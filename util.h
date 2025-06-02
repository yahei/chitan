#include <stdlib.h>

#ifndef UTIL_H
#define UTIL_H

#define MIN(a,b)        ((a) < (b) ? (a) : (b))
#define MAX(a,b)        ((a) > (b) ? (a) : (b))

void errExit(char *);
void fatal(char *);
void *xmalloc(size_t);
void *xrealloc(void *p, size_t);

#endif
