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

static void procNCCs(Term *);

#define READBUF_SIZE 16

Term *
openTerm(void)
{
	Term *term;
	char *sname;
	int slave;
	int i;

	/* 構造体の初期化 */
	term = xmalloc(sizeof(Term));
	*term = (Term){ -1, NULL, 16, 0, 0, NULL};

	term->lines = xmalloc(term->maxlines * sizeof(Line *));
	for (i = 0; i < term->maxlines; i++)
		term->lines[i] = allocLine();

	term->NCCs = xmalloc(READBUF_SIZE + 1);
	term->NCCs[0] = '\0';

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
		freeLine(term->lines[i]);
		term->lines[i] = NULL;
	}
	free(term->lines);

	free(term->NCCs);

	free(term);
}

ssize_t
readPty(Term *term)
{
	unsigned char readbuf[READBUF_SIZE], *reading;
	ssize_t size;
	char *pNCC = strchr(term->NCCs, '\0');

	size = read(term->master, readbuf, term->NCCs + READBUF_SIZE - pNCC);

	/* プロセスが終了してる場合など */
	if (size < 0)
		return size;

	for (reading = readbuf; reading - readbuf < size; reading++) {
		/* 非制御文字 */
		if (31 < *reading && *reading != 127) {
			*(pNCC++) = *reading;
			continue;
		}

		/* 制御文字 */
		*pNCC = '\0';
		procNCCs(term);
		pNCC = strchr(term->NCCs, '\0');

		switch (*reading) {
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
	}
	*pNCC = '\0';
	procNCCs(term);

	return size;
}

void
procNCCs(Term *term)
{
	char32_t decoded[READBUF_SIZE + 1];
	char *rest;
	int restlen;

	rest = u8sToU32s(decoded, term->NCCs, READBUF_SIZE);
	decoded[READBUF_SIZE] = L'\0';
	term->cursor += putU32(getLine(term, 0), term->cursor,
			decoded, u32slen(decoded));

	restlen = strlen(rest);
	memmove(term->NCCs, rest + MAX(restlen - 3, 0), MIN(restlen + 1, 4));
}

ssize_t
writePty(Term *term, char *buf, ssize_t n)
{
	ssize_t size;
	size = write(term->master, buf, n);
	return size;
}

Line *
getLine(Term *term, int index)
{
	if (index > term->lastline || index < 0 || index >= term->maxlines)
		return NULL;

	return term->lines[(term->lastline - index) % term->maxlines];
}
