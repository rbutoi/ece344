#include <math.h>
#include <assert.h>
#include "common.h"
#include "point.h"

void
point_translate(struct point *p, double x, double y)
{
	p->x += x;
	p->y += y;
}

double
point_distance(const struct point *p1, const struct point *p2)
{
	return sqrt(
		( (p1->x - p2->x) * (p1->x - p2->x) ) +
		( (p1->y - p2->y) * (p1->y - p2->y) )
		);
}

int
point_compare(const struct point *p1, const struct point *p2)
{
	struct point origin;
	point_set(&origin, 0, 0);

	double len1 = point_distance(p1, &origin);
	double len2 = point_distance(p2, &origin);

	if (len1 < len2) {
		return -1;
	} else if (len1 == len2) {
		return 0;
	} else {
		return 1;
	}
}
