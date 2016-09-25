#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "common.h"
#include "point.h"
#include "sorted_points.h"

#define FAIL_IF_EMPTY() do {									   \
		if (!sp || !sp->head) {									   \
			return 0;											   \
		}														   \
	} while (0)

int point_compare(const struct point *p1, const struct point *p2);

struct point_node {
	struct point p;
	struct point_node *next;
};

struct sorted_points {
	struct point_node *head;
};

int point_compare_tiebreaker(const struct point *p1, const struct point *p2);

struct sorted_points *
sp_init()
{
	struct sorted_points *sp;

	sp = (struct sorted_points *)malloc(sizeof(struct sorted_points));
	assert(sp);

	sp->head = NULL;

	return sp;
}

void
sp_destroy(struct sorted_points *sp)
{
	if (!sp) {
		return;
	}

	struct point_node *curr = sp->head;
	while (curr) {
		struct point_node *prev = curr;
		curr = curr->next;
		free(prev);
	}
	free(sp);
}

int
point_compare_tiebreaker(const struct point *p1, const struct point *p2)
{
	int ret = point_compare(p1, p2);
	if (ret == 0) {
		if (p1->x == p2->x) {
			ret = p1->y < p2->y ? -1 : 1;
		} else {
			ret = p1->x < p2->x ? -1 : 1;
		}
	}

	return ret;
}

int
sp_add_point(struct sorted_points *sp, double x, double y)
{
	if (!sp) {
		return 0;
	}

	struct point_node *node = (struct point_node *) malloc(sizeof(struct point_node));
	if (node) {
		point_set(&node->p, x, y);
	} else {
		return 0;
	}

	/* empty list */
	if (!sp->head) {
		node->next = NULL;
		sp->head = node;
		return 1;
	}

	/* replace head */
	if (point_compare_tiebreaker(&node->p, &sp->head->p) == -1) {
		node->next = sp->head;
		sp->head = node;
		return 1;
	}

	struct point_node *curr = sp->head->next;
	struct point_node *prev = sp->head;
	while (curr && point_compare_tiebreaker(&curr->p, &node->p) <= 0) {
		curr = curr->next;
		prev = prev->next;
	}
	prev->next = node;
	node->next = curr;
	
	return 1;
}

int
sp_remove_first(struct sorted_points *sp, struct point *ret)
{
	FAIL_IF_EMPTY();

	*ret = sp->head->p;

	struct point_node *second = sp->head->next;
	free(sp->head);
	sp->head = second;

	return 1;
}

int
sp_remove_last(struct sorted_points *sp, struct point *ret)
{
	FAIL_IF_EMPTY();

	struct point_node *curr = sp->head->next;
	struct point_node *prev = sp->head;
	if (!curr) {
		*ret = prev->p;
		free(prev);
		sp->head = NULL;
		return 1;
	}

	while (curr->next) {
		curr = curr->next;
		prev = prev->next;
	}

	*ret = curr->p;
	free(curr);
	prev->next = NULL;

	return 1;
}

int
sp_remove_by_index(struct sorted_points *sp, int index, struct point *ret)
{
	FAIL_IF_EMPTY();

	if (index == 0) {
		struct point_node *removed = sp->head;
		sp->head = removed->next;
		*ret = removed->p;
		free(removed);
		return 1;
	}

	struct point_node *curr = sp->head->next;
	struct point_node *prev = sp->head;
	while (curr && index != 1) {
		curr = curr->next;
		prev = prev->next;
		--index;
	}
	if (curr) {
		*ret = curr->p;
		prev->next = curr->next;
		free(curr);
		return 1;
	} else {
		return 0;
	}
}

int
sp_delete_duplicates(struct sorted_points *sp)
{
	int dups = 0;

	struct point_node *i = sp->head;
	while (i) {
		struct point_node *j_curr = i->next;
		struct point_node *j_prev = i;
		while (j_curr) {
			if (point_compare(&j_curr->p, &i->p) == 0) {
				++dups;
				struct point_node *del = j_curr;
				j_prev->next = j_curr = j_curr->next;
				free(del);
				continue;
			}
			j_curr = j_curr->next;
			j_prev = j_prev->next;
		}
		i = i->next;
	}

	return dups;
}

void print_sp(const struct sorted_points *sp) {
	struct point_node *curr = sp->head;
	while (curr) {
		printf("X: %f Y: %f\n", curr->p.x, curr->p.y);
		curr = curr->next;
	}
	printf("--\n");
}
