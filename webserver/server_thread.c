#include <pthread.h> 
#include "request.h"
#include "server_thread.h"
#include "common.h"

// circular Q
/* ------------------------------------------------------------ */
typedef struct {
	int *q;
	unsigned start, end, max_q_size_plus_one;
} circular_q;

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

/* ------------------------------------------------------------ */

/* globals */
circular_q req_q;
pthread_mutex_t req_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t req_full = PTHREAD_COND_INITIALIZER;
pthread_cond_t req_empty = PTHREAD_COND_INITIALIZER;

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
	/* reads file, 
	 * fills data->file_buf with the file contents,
	 * data->file_size with file size. */
	ret = request_readfile(rq);
	if (!ret)
		goto out;
	/* sends file to client */
	request_sendfile(rq);
out:
	request_destroy(rq);
	file_data_free(data);
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

	/* Lab 5: init server cache and limit its size to max_cache_size */

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
