#include <pthread.h> 
#include "request.h"
#include "server_thread.h"
#include "common.h"

struct request {
	int fd;		 /* descriptor for client connection */
	struct file_data *data;
};

/* circular Q header */

typedef struct {
	int *q;
	unsigned start, end, max_q_size_plus_one;
} circular_q;

void q_init (      circular_q *q, unsigned max_size);
int  q_full (const circular_q *q);
int  q_empty(const circular_q *q);
int  q_size (const circular_q *q);
void q_print(const circular_q *q);
void q_enq  (      circular_q *q, int a);
int  q_deq  (      circular_q *q);

/* cache header */

#ifdef DEBUG
#define BUCKETS 2
#else
#define BUCKETS 1000
#endif

typedef struct node_ {
	struct file_data *data;
	int reading;
	struct node_ *next;
} node;

node *make_node(struct file_data *data, node *next);
unsigned long hash(const char *str, int len);

node *cache_lookup(struct file_data *data);
node *cache_insert(struct file_data *data);
node *cache_lookup_or_insert(struct file_data *data);
int   cache_delete(struct file_data *data);
int   cache_evict (int amount);
void  cache_print ();

typedef struct lru_node_ {
	struct file_data *data;
	struct lru_node_ *next;
} lru_node;

lru_node *make_lru_node(struct file_data *data, lru_node *next);
void lru_use(struct file_data *data);
void lru_print();

/* globals */

circular_q req_q;
pthread_mutex_t req_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t req_full = PTHREAD_COND_INITIALIZER;
pthread_cond_t req_empty = PTHREAD_COND_INITIALIZER;

node **cache_buckets = NULL;
unsigned cache_usage = 0;
pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

lru_node *lru_list_head = NULL;

void *worker(void *sv_v);

struct server {
	int nr_threads;
	int max_requests;
	int max_cache_size;
};

/* static functions */

/* initialize file data */
static struct file_data *
file_data_init(void)
{
	struct file_data *data;

	data = Malloc(sizeof(struct file_data));
	data->file_name = NULL;
	data->file_buf = NULL;
	data->file_size = 0;
	return data;
}

/* free all file data */
static void
file_data_free(struct file_data *data)
{
	free(data->file_name);
	free(data->file_buf);
	free(data);
}

static void
do_server_request(struct server *sv, int connfd)
{
	int ret;
	struct request *rq;
	struct file_data *data;

	data = file_data_init();

	/* fills data->file_name with name of the file being requested */
	rq = request_init(connfd, data);
	if (!rq) {
		file_data_free(data);
		return;
	}

#ifdef DEBUG
	printf("request for %s\n", data->file_name);
#endif

	/* check cache for file */

	pthread_mutex_lock(&cache_lock);
	node *cached = cache_lookup(data);
	if (cached) {
		cached->reading = 1;
		file_data_free(data);
		rq->data = cached->data;
		pthread_mutex_unlock(&cache_lock);

		request_sendfile(rq);

		pthread_mutex_lock(&cache_lock);
		lru_use(cached->data);
		cached->reading = 0;
		pthread_mutex_unlock(&cache_lock);

	} else {
		pthread_mutex_unlock(&cache_lock);
		ret = request_readfile(rq);

		if (ret) {
			request_sendfile(rq);

			pthread_mutex_lock(&cache_lock);
			cached = cache_lookup(data);
			if (!cached) {
				int file_too_big_for_cache = rq->data->file_size > sv->max_cache_size;
				int evict_amount = cache_usage + rq->data->file_size - sv->max_cache_size;

				if (!file_too_big_for_cache && evict_amount > 0) {
					/* adding would overfill cache, need to evict */
					if (cache_evict(evict_amount) > 0) {
						/* still have to evict but can't due to reading files */
						file_too_big_for_cache = 1;
					}
				}

				if (!file_too_big_for_cache) {
					cache_insert(data);
					lru_use(data);
				}
			}
			pthread_mutex_unlock(&cache_lock);
		}
	}

	request_destroy(rq);

#ifdef DEBUG
	cache_print();
	lru_print();
	fflush(stdout);
#endif
}

/* entry point functions */

struct server *
server_init(int nr_threads, int max_requests, int max_cache_size)
{
	struct server *sv;

	sv = Malloc(sizeof(struct server));
	sv->nr_threads = nr_threads;
	sv->max_requests = max_requests;
	sv->max_cache_size = max_cache_size;

	q_init(&req_q, max_requests);

	int i;
	for (i = 0; i < nr_threads; ++i) {
		pthread_t t;
		int ret = pthread_create(&t, NULL, &worker, sv);
		assert(!ret);
	}

	/* cache */
	cache_buckets = (node **) calloc(BUCKETS, sizeof(node *));
	assert(cache_buckets);

	return sv;
}

void
server_request(struct server *sv, int connfd)
{
	if (sv->nr_threads == 0) { /* no worker threads */
		do_server_request(sv, connfd);
	} else {
		// produce
		pthread_mutex_lock(&req_lock);

		while (q_full(&req_q)) {
			pthread_cond_wait(&req_full, &req_lock);
		}

		q_enq(&req_q, connfd);

		if (!q_empty(&req_q)) {
			pthread_cond_signal(&req_empty);
		}

		pthread_mutex_unlock(&req_lock);
	}
}

void *worker(void *sv_v)
{
	struct server *sv = (struct server *) sv_v;
	while (1) {
		// consume
		pthread_mutex_lock(&req_lock);

		while (q_empty(&req_q)) {
			pthread_cond_wait(&req_empty, &req_lock);
		}

		int fd = q_deq(&req_q);

		if (!q_full(&req_q)) {
			pthread_cond_signal(&req_full);
		}

		pthread_mutex_unlock(&req_lock);

		do_server_request(sv, fd);
	}

	return NULL;
}

/* cache implementation */

node *
make_node(struct file_data *data, node *next)
{
	node *newnode = (node *) malloc(sizeof(node));
	assert(newnode);

	newnode->data = data;
	newnode->reading = 0;
	newnode->next = next;
	return newnode;
}

/* djb2 string hash algorithm from http://www.cse.yorku.ca/~oz/hash.html */ 
unsigned long
hash(const char *str, int len)
{
	unsigned long hash = 5381;
	int i;

	for (i = 0; i < len; ++i) {
		hash = ((hash << 5) + hash) + str[i]; /* hash * 33 + c */
	}

	return hash;
}

node *
cache_lookup(struct file_data *data)
{
	int len = strlen(data->file_name);
	int hash_in_bucket = hash(data->file_name, len) % BUCKETS;

	node *curr = cache_buckets[hash_in_bucket];

	while (curr) {
		if (!strncmp(curr->data->file_name, data->file_name, len)) {
			return curr;
		}
		curr = curr->next;
	}
	return NULL;
}

node *
cache_insert(struct file_data *data)
{
	int len = strlen(data->file_name);
	int hash_in_bucket = hash(data->file_name, len) % BUCKETS;

	node **curr = &cache_buckets[hash_in_bucket];

	while (*curr) {
		assert(!!strncmp((*curr)->data->file_name, data->file_name, len));
		
		curr = &((*curr)->next);
	}

	cache_usage += data->file_size;
	*curr = make_node(data, NULL);

	return *curr;
}

node *cache_lookup_or_insert(struct file_data *data)
{
	int len = strlen(data->file_name);
	int hash_in_bucket = hash(data->file_name, len) % BUCKETS;

	node **curr = &cache_buckets[hash_in_bucket];

	while (*curr) {
		if (!strncmp((*curr)->data->file_name, data->file_name, len)) {
			return *curr;
		}
		
		curr = &((*curr)->next);
	}

	/* cache_usage += data->file_size; */
	*curr = make_node(data, NULL);

	return *curr;
}

int
cache_delete(struct file_data *data)
{
	int deleted = 0;
	int len = strlen(data->file_name);
	int hash_in_bucket = hash(data->file_name, len) % BUCKETS;

	node **head = &cache_buckets[hash_in_bucket];
	assert(*head);
	
	if (!strncmp((*head)->data->file_name, data->file_name, len)) {
		/* delete first */

		if ((*head)->reading) {
			return -1;
		}
		cache_usage -= (*head)->data->file_size;
		deleted = (*head)->data->file_size;
		node *sacrificial = *head;
		*head = (*head)->next;
		file_data_free(sacrificial->data);
		free(sacrificial);
	} else {
		node *prev = *head;
		node *curr = prev->next;

		while (!!strncmp(curr->data->file_name, data->file_name, len)) {
			prev = prev->next;
			curr = curr->next;

			assert(curr);
		}

		if (curr->reading) {
			return -1;
		}
		prev->next = curr->next;
		cache_usage -= curr->data->file_size;
		deleted = curr->data->file_size;
		file_data_free(curr->data);
		free(curr);
	}

	return deleted;
}

int cache_evict(int amount)
{
	int deleted;
	while (lru_list_head && amount > 0) {
		deleted = cache_delete(lru_list_head->data);
		if (deleted == -1) continue;
		assert(deleted);
		amount -= deleted;

		lru_node *sacrificial = lru_list_head;
		lru_list_head = lru_list_head->next;
		free(sacrificial);
	}

	/* if successfully evicted from head, or evicted everything from head */
	if (amount <= 0 || !lru_list_head) {
		return amount;
	}

	lru_node *prev = lru_list_head;
	assert(prev);
	lru_node *curr = prev->next;

	while (curr && amount > 0) {
		deleted = cache_delete(curr->data);
		if (deleted == -1) continue;
		assert(deleted);

		if (deleted != -1) {
			amount -= deleted;

			curr = curr->next;
			prev->next = curr;
			free(curr);
		} else {
			prev = prev->next;
			curr = curr->next;
		}
	}

	return amount;
}

void cache_print()
{
	printf("cache\n");
	int i;
	for (i = 0; i < BUCKETS; ++i) {
		printf("\tbucket %d\n", i);
		node *curr = cache_buckets[i];
		while (curr) {
			if (curr->data->file_name) {
				printf("\t\t%s:%d\n", curr->data->file_name, curr->reading);
			}
			curr = curr->next;
		}
	}
}

/* LRU */

lru_node *make_lru_node(struct file_data *data, lru_node *next)
{
	lru_node *new_lru_node = (lru_node *) malloc(sizeof(lru_node));
	assert(new_lru_node);

	new_lru_node->data = data;
	new_lru_node->next = next;
	return new_lru_node;
}

void lru_use(struct file_data *data)
{
	int len = strlen(data->file_name);

	if (lru_list_head) {
		lru_node *match = NULL;
		lru_node *prev = lru_list_head; 
		lru_node *curr = lru_list_head->next;

		if (!strncmp(prev->data->file_name, data->file_name, len)) {
			match = prev;

			lru_list_head = curr;
		}

		while (curr) {
			if (!strncmp(curr->data->file_name, data->file_name, len)) {
				assert(!match);
				match = curr;

				prev->next = curr->next;
			} else {
				prev = prev->next;
			}
			curr = curr->next;
		}

		if (match) {
			if (lru_list_head) {
				prev->next = match;
			} else {
				lru_list_head = match;
			}
			match->next = NULL;
		} else {
			prev->next = make_lru_node(data, NULL);
		}
	} else {
		lru_list_head = make_lru_node(data, NULL);
	}
}

void lru_print()
{
	printf("lru\n");
	lru_node *n = lru_list_head;
	while (n) {
		printf("\t\t%s\n", n->data->file_name);
		n = n->next;
	}
}

/* circular Q implementation */

void q_init(circular_q *q, unsigned max_size)
{
	q->max_q_size_plus_one = max_size + 1;
	q->q = malloc(sizeof(int) * q->max_q_size_plus_one);
	q->start = q->end = 0;
}

int q_full(const circular_q *q)
{
	return q->start == ((q->end + 1) % q->max_q_size_plus_one);
}

int q_empty(const circular_q *q)
{
	return q->start == q->end;
}

int q_size(const circular_q *q)
{
	unsigned size = q->end - q->start;
	return size < 0 ? size + q->max_q_size_plus_one : size; 
}

void q_print(const circular_q *q)
{
	unsigned i = q->start;
	while (i != q->end) {
		printf("%d ", q->q[i]);
		i = (i + 1) % q->max_q_size_plus_one;
	}
	printf("\n");
}

void q_enq(circular_q *q, int a)
{
	assert( !q_full(q) );

	q->q[q->end] = a;
	q->end = (q->end + 1) % q->max_q_size_plus_one;
}

int q_deq(circular_q *q)
{
	assert( !q_empty(q) );

	int ret = q->q[q->start];
	q->start = (q->start + 1) % q->max_q_size_plus_one;
	return ret;
}

/*
	pthread_mutex_lock(&cache_lock);
	node *cached = cache_lookup_or_insert(data);
	if (cached->data->file_buf) {
		cached->reading = 1;
		lru_use(cached->data);
		pthread_mutex_unlock(&cache_lock);

		file_data_free(data);
		rq->data = cached->data;

		request_sendfile(rq);

		pthread_mutex_lock(&cache_lock);
		cached->reading = 0;
		pthread_mutex_unlock(&cache_lock);
	} else {
		// read into another data struct
		struct file_data *f = file_data_init();
		f->file_name = cached->data->file_name;
		rq->data = f;
		cached->reading = 1;
		pthread_mutex_unlock(&cache_lock);

		ret = request_readfile(rq);

		if (ret) {
			pthread_mutex_lock(&cache_lock);
			cached->reading = 0;

			int file_too_big_for_cache = rq->data->file_size > sv->max_cache_size;
			int evict_amount = cache_usage + rq->data->file_size - sv->max_cache_size;

			if (!file_too_big_for_cache && evict_amount > 0) {
				// adding would overfill cache, need to evict
				if (cache_evict(evict_amount) > 0) {
					// still have to evict but can't due to reading files
					file_too_big_for_cache = 1;
				}
			}

			if (!file_too_big_for_cache) {
				cached->reading = 1;
				cached->data->file_buf = rq->data->file_buf;
				lru_use(cached->data);
			}
			pthread_mutex_unlock(&cache_lock);

			request_sendfile(rq);

			pthread_mutex_lock(&cache_lock);
			if (!file_too_big_for_cache) {
				cached->reading = 0;
				cached->data->file_size = rq->data->file_size;
				cache_usage += rq->data->file_size;
				free(f);
			} else {
				ret = cache_delete(data);
				assert(ret == 0);
			}
			pthread_mutex_unlock(&cache_lock);
		} else {
			assert(0);
		}
	}
*/
