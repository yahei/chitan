#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "util.h"

int memcnt;

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
