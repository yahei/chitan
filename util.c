#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

void
errExit(const char *message)
{
	if (message)
		fputs(message, stderr);
	fputs(strerror(errno), stderr);
	fputs("\n", stderr);
	exit(1);
}

void
fatal(const char *message)
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

char *
strtok2(char *str, char *delim)
{
	static char *last;

	if (str == NULL)
		str = last;

	if (str && (last = strpbrk(str, delim))) {
		*last = '\0';
		last++;
	}

	return str;
}
