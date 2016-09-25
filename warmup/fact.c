#include "common.h"
#include <ctype.h>

int fact(int n)
{
	if (n <= 1) {
		return 1;
	}
	return n * fact(n - 1);
}

int
main(int argc, char **argv)
{
	int in;

	if (argc < 2) {
		printf("Huh?\n");
		return 1;
	}
	char *end;
	in = (int) strtol(argv[1], &end, 0);

	if (in < 1 || *end) {
		printf("Huh?\n");
		return 1;
	}
	if (in > 12) {
		printf("Overflow\n");
		return 1;
	}

	printf("%d\n", fact(in));

	return 0;
}
