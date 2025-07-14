#ifndef UTIL_H
#define UTIL_H

#define MIN(a,b)        ((a) < (b) ? (a) : (b))
#define MAX(a,b)        ((a) > (b) ? (a) : (b))
#define CLIP(a,b,c)     (MIN(MAX(a, b), c))
#define BETWEEN(a,b,c)  ((b) <= (a) && (a) < (c))

void errExit(const char *);
void fatal(const char *);
void *xmalloc(size_t);
void *xrealloc(void *, size_t);
char *strtok2(char *, char *);

#endif
