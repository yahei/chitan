#include <stdio.h>
#include <stdlib.h>

#ifndef UTIL_H
#define UTIL_H

#define MIN(a,b)        ((a) < (b) ? (a) : (b))
#define MAX(a,b)        ((a) > (b) ? (a) : (b))

void errExit(char *);

#endif

extern int memcnt;

inline static void *
_malloc(size_t size)
{
	void *p = malloc(size);
	if (p == NULL)
		errExit("malloc failed.\n");
	memcnt++;
	return p;
}

inline static void
_free(void *p)
{
	free(p);
	if (p)
		memcnt--;
}
