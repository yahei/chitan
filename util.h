#ifndef UTIL_H
#define UTIL_H

#define MIN(a,b)        ((a) < (b) ? (a) : (b))
#define MAX(a,b)        ((a) > (b) ? (a) : (b))

void errExit(const char *);
void fatal(const char *);
void *xmalloc(size_t);
void *xrealloc(void *p, size_t);

#endif
