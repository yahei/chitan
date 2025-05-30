#include <string.h>
#include <errno.h>

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
