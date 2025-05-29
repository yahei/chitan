#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include "tty.h"
#include "util.h"

/*
 * Term
 *
 * ターミナルのセッションとバッファを管理する
 */

struct Term {
	int master, slave;      /* 疑似端末のファイルディスクリプタ */
	Line **lines;           /* バッファ */
	int maxlines;           /* バッファの最大行数*/
	int lastline;           /* 今の最終行 */
};

Term *
newTerm(void)
{
	Term *term;
	char *sname;
	int i;

	term = _malloc(sizeof(Term));

	/* 行数と最終行の設定 */
	term->maxlines = 32;
	term->lastline = 0;

	/* バッファの作成 */
	term->lines = _malloc(term->maxlines * sizeof(void *));
	for (i = 0; i < term->maxlines; i++)
		term->lines[i] = NULL;

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
		if (execl("/usr/bin/ed", "tty.c") < 0)
			exit(1);
		break;
	default: /* 本体側 */
		close(term->slave);
	}

	return term;

FAIL:
	deleteTerm(term);
	return NULL;
}

void
deleteTerm(Term *term)
{
	int i;

	if (term == NULL)
		return;

	/* 疑似端末をcloseする */
	if (term->master >= 0)
		close(term->master);
	term->slave  = -1;
	term->master = -1;

	/* バッファを解放 */
	for(i = 0; i < term->maxlines; i++) {
		deleteLine(term->lines[i]);
		term->lines[i] = NULL;
	}
	_free(term->lines);

	_free(term);
}

int
getfdTerm(Term *term)
{
	return term->master;
}

void
readpty(Term *term)
{
	char buf[1024];

	read(term->master, buf, sizeof(buf));
	term->lines[0] = newLine();
	setmbLine(term->lines[0], buf);
}

int
getlastlineTerm(Term *term)
{
	return term->lastline;
}

Line *
getlineTerm(Term *term, int num)
{
	return term->lines[num];
}


/*
 * Line
 *
 * バッファ一行分の情報を管理する
 */

struct Line {
	char *str;      /* UTF8文字列 */
};

Line *
newLine(void)
{
	Line *line = _malloc(sizeof(Line));

	/* 初期値としてnull文字をセット */
	line->str = _malloc(1);
	line->str = '\0';

	return line;

FAIL:
	deleteLine(line);
	return NULL;
}

void
deleteLine(Line *line)
{
	if (line == NULL)
		return;

	/* 文字列を解放 */
	_free(line->str);
	line->str = NULL;

	_free(line);
}

void
setmbLine(Line *line, char *str)
{
	line->str = realloc(line->str, strlen(str));
	strcpy(line->str, str);
}

char *
getmbLine(Line *line)
{
	return line->str;
}
