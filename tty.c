#define _XOPEN_SOURCE 600

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
	int cursor;             /* カーソル位置 */
};

Term *
newTerm(void)
{
	Term *term;
	char *sname;
	int i;

	term = xmalloc(sizeof(Term));

	/* 行数と最終行とカーソル位置の設定 */
	term->maxlines = 2 << 15;
	term->lastline = 0;
	term->cursor = 0;

	/* バッファの作成 */
	term->lines = xmalloc(term->maxlines * sizeof(Line *));
	for (i = 0; i < term->maxlines; i++)
		term->lines[i] = NULL;
	term->lines[term->lastline] = newLine();

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
		if (setsid() < 0)
			fatal("setsid failed.\n");
		if (execlp("sh", "sh", NULL) < 0)
			fatal("exec failed.\n");
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
	for (i = 0; i < term->maxlines; i++) {
		deleteLine(term->lines[i]);
		term->lines[i] = NULL;
	}
	free(term->lines);

	free(term);
}

int
getfdTerm(Term *term)
{
	return term->master;
}

ssize_t
readptyTerm(Term *term)
{
	char buf[1024];
	ssize_t size;
	char *head, *tail;

	size = read(term->master, buf, sizeof(buf));

	/* 
	 * 何らかのエラー
	 * たぶんプロセスが終了している
	 * 他の原因もあり得るので、後でちゃんとする
	 */
	if (size < 0) {
		return size;
	}

	/* 受け取った文字列を数値で表示 */
	printf("read(%ld):", (long)size);
	/*
	for (int i=0; i < size; i++)
		printf("%d,", buf[i]);
	*/
	printf("\n");

	/* 改行があったら次の行に進むとかの処理 */
	for (head = tail = buf; tail < buf + size; tail++) {
		switch (*tail) {
		case 9:  /* HT */
			overwritembLine(term->lines[term->lastline],
					head, tail - head, term->cursor);
			term->cursor += tail - head;
			overwritembLine(term->lines[term->lastline],
					"    ", 4, term->cursor);
			term->cursor += 4;
			head = tail + 1;
			break;
		case 10: /* LF */
			overwritembLine(term->lines[term->lastline],
					head, tail - head, term->cursor);
			term->lastline++;
			if (term->lines[term->lastline] == NULL)
				term->lines[term->lastline] = newLine();
			head = tail + 1;
			break;

		case 13: /* CR */
			overwritembLine(term->lines[term->lastline],
					head, tail - head, term->cursor);
			term->cursor = 0;
			head = tail + 1;
			break;
		}
	}
	overwritembLine(term->lines[term->lastline],
			head, tail - head, term->cursor);
	term->cursor += tail - head;

	return size;
}

ssize_t
writeptyTerm(Term *term, char *buf, ssize_t n)
{
	ssize_t size;
	size = write(term->master, buf, n);
	return size;
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
	int len;        /* 文字数 */
};

Line *
newLine(void)
{
	Line *line = xmalloc(sizeof(Line));
	line->str = NULL;
	line->len = 0;
	setmbLine(line, "", 0);
	return line;
}

void
deleteLine(Line *line)
{
	if (line == NULL)
		return;

	/* 文字列を解放 */
	free(line->str);
	line->str = NULL;

	free(line);
}

void
setmbLine(Line *line, char *str, int size)
{
	line->str = xrealloc(line->str, size + 1);
	strncpy(line->str, str, size);
	line->str[size] = '\0';
}

char *
getmbLine(Line *line)
{
	return line->str;
}

void
overwritembLine(Line *line, char *str, int size, int pos)
{
	int newlen = MAX(pos + size, line->len);

	/* 必要なら文字列を伸ばす */
	if (line->len < newlen) {
		line->str = xrealloc(line->str, newlen + 1);
		line->str[newlen] = '\0';
		line->len = newlen;
	}

	/* 文字列を書き込む */
	strncpy(&line->str[pos], str, size);
}

int
getcursorTerm(Term *term)
{
	return term->cursor;
}
