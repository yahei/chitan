#define _XOPEN_SOURCE 600

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "term.h"
#include "util.h"

/*
 * Term
 *
 * 疑似端末とバッファを管理する
 */

Term *
openTerm(void)
{
	Term *term;
	char *sname;
	int slave;
	int i;

	/* 構造体の初期化 */
	term = xmalloc(sizeof(Term));
	*term = (Term){ -1, NULL, 2 << 15, 0, 0 };
	term->lines = xmalloc(term->maxlines * sizeof(Line *));
	for (i = 0; i < term->maxlines; i++)
		term->lines[i] = NULL;
	term->lines[term->lastline] = newLine();

	/* 疑似端末を開く */
	errno = -1;
	if ((term->master = posix_openpt(O_RDWR)) < 0)
		goto FAIL;
	if ((sname = ptsname(term->master)) == NULL)
		goto FAIL;
	if (grantpt(term->master) < 0)
		goto FAIL;
	if (unlockpt(term->master) < 0)
		goto FAIL;
	if ((slave = open(sname, O_RDWR)) < 0)
		goto FAIL;

	/* slave側でプロセスを起動 */
	switch (fork()) {
	case -1:/* 失敗 */
		goto FAIL;
		break;
	case 0: /* プロセス側 */
		close(term->master);
		dup2(slave, 0);
		dup2(slave, 1);
		dup2(slave, 2);
		if (setsid() < 0)
			fatal("setsid failed.\n");
		if (execlp("sh", "sh", NULL) < 0)
			fatal("exec failed.\n");
		break;
	default: /* 本体側 */
		close(slave);
	}

	return term;

FAIL:
	closeTerm(term);
	return NULL;
}

void
closeTerm(Term *term)
{
	int i;

	if (term == NULL)
		return;

	/* 疑似端末を閉じる */
	if (term->master >= 0)
		close(term->master);

	/* バッファを解放 */
	for (i = 0; i < term->maxlines; i++) {
		deleteLine(term->lines[i]);
		term->lines[i] = NULL;
	}
	free(term->lines);

	free(term);
}

ssize_t
readPty(Term *term)
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
			overwriteUtf8(term->lines[term->lastline],
					head, tail - head, term->cursor);
			term->cursor += tail - head;
			overwriteUtf8(term->lines[term->lastline],
					"    ", 4, term->cursor);
			term->cursor += 4;
			head = tail + 1;
			break;
		case 10: /* LF */
			overwriteUtf8(term->lines[term->lastline],
					head, tail - head, term->cursor);
			term->lastline++;
			if (term->lines[term->lastline] == NULL)
				term->lines[term->lastline] = newLine();
			head = tail + 1;
			break;

		case 13: /* CR */
			overwriteUtf8(term->lines[term->lastline],
					head, tail - head, term->cursor);
			term->cursor = 0;
			head = tail + 1;
			break;
		}
	}
	overwriteUtf8(term->lines[term->lastline],
			head, tail - head, term->cursor);
	term->cursor += tail - head;

	return size;
}

ssize_t
writePty(Term *term, char *buf, ssize_t n)
{
	ssize_t size;
	size = write(term->master, buf, n);
	return size;
}
