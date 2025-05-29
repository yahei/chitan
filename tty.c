#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "tty.h"

#define MIN(a,b)        ((a) < (b) ? (a) : (b))
#define MAX(a,b)        ((a) > (b) ? (a) : (b))

/* Term */

struct Term {
	int master, slave;      /* 疑似端末のファイルディスクリプタ */
	Line **lines;           /* バッファ */
	int maxlines;           /* バッファの最大行数*/
	int lastline;           /* 今の最終行 */
};

Term *
openterm(void)
{
	Term *term;
	char *sname;

	term = malloc(sizeof(Term));
	if (term == NULL)
		goto FAIL;

	term->maxlines = 32;
	term->lastline = 0;

	term->lines = malloc(term->maxlines * sizeof(void *));
	if (term->lines == NULL)
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
		// 読んでバッファに記録
		char buf[64];
		read(term->master, buf, sizeof(buf));
		term->lines[0] = createline();
		setutf8(term->lines[0], buf);
		// バッファから読み出してprintf
		printf("%s\n", getutf8(term->lines[term->lastline]));
	}

	return term;

FAIL:
	closeterm(term);
	return NULL;
}

void
closeterm(Term *term)
{
	int i;

	if (term == NULL)
		return;

	if (term->master >= 0)
		close(term->master);

	for(i = MAX(0, term->lastline - term->maxlines);
			i < term->lastline;
			i++) {
		if (term->lines[i]) {
			deleteline(term->lines[i]);
			term->lines[i] = NULL;
		}
	}

	term->slave  = -1;
	term->master = -1;
	free(term);
}

/* Line */

struct Line {
	char *str;      /* UTF8文字列 */
};

Line *
createline(void)
{
	Line *line = malloc(sizeof(Line));
	if (line == NULL)
		goto FAIL;

	line->str = malloc(1);
	if (line->str == NULL)
		goto FAIL;
	line->str = '\0';

	return line;

FAIL:
	deleteline(line);
	return NULL;
}

void
deleteline(Line *line)
{
	if (line == NULL)
		return;

	free(line->str);
	line->str = NULL;

	free(line);
}

void
setutf8(Line *line, char *str)
{
	line->str = realloc(line->str, strlen(str));
	strcpy(line->str, str);
}

char *
getutf8(Line *line)
{
	return line->str;
}
