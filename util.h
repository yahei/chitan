#ifndef UTIL_H
#define UTIL_H

#define MIN(a,b)        ((a) < (b) ? (a) : (b))
#define MAX(a,b)        ((a) > (b) ? (a) : (b))
#define CLIP(a,b,c)     (MIN(MAX(a, b), c))
#define BETWEEN(a,b,c)  ((b) <= (a) && (a) < (c))
#define INIT(arr, val) do {\
	for (int i = sizeof(arr) / sizeof((arr)[0]); 0 < i; i--)\
		(arr)[i - 1] = (val);\
} while (0);
#define PUSH_BACK(arr, len, new) do {\
	(arr) = xrealloc((arr), ++(len) * sizeof((arr)[0]));\
	(arr)[(len) - 1] = (new);\
} while (0);
#define timespecadd(a, b, c) do {\
	(c)->tv_sec  = (a)->tv_sec  + (b)->tv_sec;\
	(c)->tv_nsec = (a)->tv_nsec + (b)->tv_nsec;\
	(c)->tv_sec  += 1000000000 <= (c)->tv_nsec ? 1 : 0;\
	(c)->tv_nsec -= 1000000000 <= (c)->tv_nsec ? 1000000000 : 0;\
} while (0);
#define timespecsub(a, b, c) do {\
	(c)->tv_sec  = (a)->tv_sec  - (b)->tv_sec;\
	(c)->tv_nsec = (a)->tv_nsec - (b)->tv_nsec;\
	(c)->tv_sec  -= (a)->tv_nsec < (b)->tv_nsec ? 1 : 0;\
	(c)->tv_nsec += (a)->tv_nsec < (b)->tv_nsec ? 1000000000 : 0;\
} while (0);
#define timespeccmp(a, b, CMP) ((a)->tv_sec == (b)->tv_sec ?\
		(a)->tv_nsec CMP (b)->tv_nsec : (a)->tv_sec CMP (b)->tv_sec)

void errExit(const char *);
void fatal(const char *);
void *xmalloc(size_t);
void *xrealloc(void *, size_t);
char *strtok2(char *, char *);

#endif
