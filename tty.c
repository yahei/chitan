#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "tty.h"

struct Term {
	int master, slave;      /* 疑似端末のファイルディスクリプタ */
};

Term *
openterm(void)
{
	Term *term;
	char *sname;

	if ((term = malloc(sizeof(Term))) == NULL)
		goto FAIL;

	/* 疑似端末をopenする */
	errno = -1;
	if ((term->master = posix_openpt(O_RDWR)) < 0)
		goto FAIL;
	if ((sname = ptsname(term->master)) == NULL)
		goto FAIL;
	if (grantpt(term->master) < 0)
		goto FAIL;
	if (unlockpt(term->master) < 0)
		goto FAIL;
	if ((term->slave = open(sname, O_RDWR)) < 0)
		goto FAIL;

	/* slave側でフォアグラウンドプロセスを起動 */
	switch (fork()) {
	case -1:/* 失敗 */
		goto FAIL;
		break;
	case 0: /* プロセス側 */
		close(term->master);
		dup2(term->slave, 0);
		dup2(term->slave, 1);
		dup2(term->slave, 2);
		if (execl("/usr/bin/echo", "テストメッセージ") < 0)
			exit(1);
		break;
	default: /* 本体側 */
		close(term->slave);
		char buf[64];
		read(term->master, buf, sizeof(buf));
		printf("%s\n", buf);
	}

	return term;

FAIL:
	closeterm(term);
	return NULL;
}

void
closeterm(Term *term)
{
	if (term == NULL)
		return;

	if (term->master >= 0)
		close(term->master);

	term->slave  = -1;
	term->master = -1;
	free(term);
}
