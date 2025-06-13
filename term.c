#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "term.h"
#include "util.h"

/*
 * Term
 *
 * 疑似端末とログを管理する
 */

static char *procNCCs(Term *, char *);
static char *procCC(Term *, char *);

#define READ_SIZE 16

Term *
openTerm(void)
{
	Term *term;
	char *sname;
	int slave;
	int i;

	/* 構造体の初期化 */
	term = xmalloc(sizeof(Term));
	*term = (Term){ -1, NULL, 16, 0, 0, NULL, 0};

	term->lines = xmalloc(term->maxlines * sizeof(Line *));
	for (i = 0; i < term->maxlines; i++)
		term->lines[i] = allocLine();

	term->readbuf = xmalloc(1);
	term->readbuf[0] = '\0';

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
	if (0 <= term->master)
		close(term->master);

	/* バッファを解放 */
	for (i = 0; i < term->maxlines; i++) {
		freeLine(term->lines[i]);
		term->lines[i] = NULL;
	}
	free(term->lines);

	free(term->readbuf);

	free(term);
}

ssize_t
readPty(Term *term)
{
	char *reading, *rest, *tail;
	ssize_t size;

	/* バッファサイズを調整してread */
	term->readbuf = xrealloc(term->readbuf, term->rblen + READ_SIZE + 1);
	size = read(term->master, term->readbuf + term->rblen, READ_SIZE);
	tail = term->readbuf + term->rblen + size;
	*tail = '\0';

	/*
	 * readしたデータの途中に\0が含まれている場合があり、
	 * \0は文字列の終わりを意味しないことに注意
	 * 終了判定はtailの位置まで読んだかどうかで行う必要がある
	 * tailに\0を置いているのはUTF8をデコードする関数に終端を知らせるため
	 */

	/* プロセスが終了してる場合など */
	if (size < 0)
		return size;

	for (reading = term->readbuf; reading < tail;) {
		/* 非制御文字列を処理 */
		rest = procNCCs(term, reading);
		if (reading != rest) {
			memmove(term->readbuf, rest, tail - rest + 1);
			tail -= rest - term->readbuf;
			reading = term->readbuf;
			continue;
		}

		/* 制御文字を処理 */
		rest = procCC(term, reading);
		if (reading != rest) {
			memmove(term->readbuf, rest, tail - rest + 1);
			tail -= rest - term->readbuf;
			reading = term->readbuf;
			continue;
		}

		reading++;
	}
	term->rblen = tail - term->readbuf;

	return size;
}

char *
procNCCs(Term *term, char *head)
{
	/*
	 * headからtailまでのUTF8文字列をデコードしてLineに書き込む
	 * 変換できなかったバイト列の先頭を指すポインタを返す
	 */
	const int len = strlen(head) + 1;
	char32_t decoded[len];
	char *rest;

	rest = u8sToU32s(decoded, head, len);
	term->cursor += putU32(getLine(term, 0), term->cursor,
			decoded, u32slen(decoded));

	return rest;
}

char *
procCC(Term *term, char *head)
{
	/*
	 * 制御文字を1文字処理する
	 * 後続のバイト列の先頭を指すポインタを返す
	 */

	/* 制御文字じゃなかったら何もしない */
	if (*head < 0 || (31 < *head && *head != 127))
		return head;

	switch (*head) {
	case 0:  /* NUL */
		return head + 1;
	case 9:  /* HT */
		term->cursor += 8 - term->cursor % 8;
		break;
	case 10: /* LF */
		deleteTrail(getLine(term, 0));
		term->lastline++;
		putU32(getLine(term, 0), 0, (char32_t *)L"\0", 1);
		break;
	case 13: /* CR */
		term->cursor = 0;
		break;
	}

	return head + 1;
}

ssize_t
writePty(Term *term, char *buf, ssize_t n)
{
	ssize_t size;
	size = write(term->master, buf, n);
	return size;
}

Line *
getLine(Term *term, unsigned int index)
{
	if (term->maxlines <= index)
		return NULL;
	if (term->lastline < index)
		return NULL;

	return term->lines[(term->lastline - index) % term->maxlines];
}
