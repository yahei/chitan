#include <stdio.h>
#include <stdlib.h>

#define MIN(a,b)        ((a) < (b) ? (a) : (b))
#define MAX(a,b)        ((a) > (b) ? (a) : (b))

extern int memcnt;

inline static void *
_malloc(size_t size)
{
	void *p = malloc(size);
	if (p == NULL) {
		fprintf(stderr, "malloc failed.\n");
		exit(1);
	}

	memcnt++;
	printf("malloc. cnt:%d\n", memcnt);

	return p;
}

inline static void
_free(void *p)
{
	free(p);

	if (p)
		memcnt--;
	printf("free. cnt:%d\n", memcnt);
}
