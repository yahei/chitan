#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "util.h"

void
errExit(char *message)
{
	if (message)
		fputs(message, stderr);
	fputs(strerror(errno), stderr);
	fputs("\n", stderr);
	exit(1);
}

void
fatal(char *message)
{
	if (message)
		fputs(message, stderr);
	exit(1);
}

void *
xmalloc(size_t size)
{
	void *p = malloc(size);
	if (p == NULL)
		errExit("malloc failed.\n");
	return p;
}

void *
xrealloc(void *p, size_t size)
{
	void *p2 = realloc(p, size);
	if (p2 == NULL)
		errExit("realloc failed.\n");
	return p2;
}
