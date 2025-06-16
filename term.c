#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
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

#define READ_SIZE 16

static const char *procNCCs(Term *, const char *);
static const char *procCC(Term *, const char *, const char *);
static const char *procESC(Term *, const char *, const char *);
static const char *procCSI(Term *, const char *, const char *);
static const char *procCStr(Term *, const char *, const char *);
static const char *procSOS(Term *, const char *, const char *);

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
	const char *reading, *rest, *tail;
	ssize_t size;

	term->readbuf = xrealloc(term->readbuf, term->rblen + READ_SIZE + 1);
	size = read(term->master, term->readbuf + term->rblen, READ_SIZE);
	tail = term->readbuf + term->rblen + size;
	*(char *)tail = '\0';

	/* プロセスが終了してる場合など */
	if (size < 0)
		return size;

	for (reading = term->readbuf; reading < tail;) {
		rest = procNCCs(term, reading);
		if (reading != rest) {
			memmove(term->readbuf, rest, tail - rest + 1);
			tail -= rest - term->readbuf;
			reading = term->readbuf;
			continue;
		}

		rest = procCC(term, reading, tail);
		if (rest == NULL) {
			break;
		} else if (reading != rest) {
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

const char *
procNCCs(Term *term, const char *head)
{
	const int len = strlen(head) + 1;
	char32_t decoded[len];
	const char *rest;

	rest = u8sToU32s(decoded, head, len);
	term->cursor += putU32(getLine(term, 0), term->cursor,
			decoded, u32slen(decoded));

	return rest;
}

const char *
procCC(Term *term, const char *head, const char *tail)
{
	if (!BETWEEN(*head, 0x00, 0x20) && *head != 0x7f)
		return head;

	/* C0 基本集合 */
	switch (*head) {
	case 0x00:  /* NUL */
		break;

	case 0x09:  /* HT */
		term->cursor += 8 - term->cursor % 8;
		break;

	case 0x0a: /* LF */
		deleteTrail(getLine(term, 0));
		term->lastline++;
		putU32(getLine(term, 0), 0, (char32_t *)L"\0", 1);
		break;

	case 0x0d: /* CR */
		term->cursor = 0;
		break;

	case 0x1b: /* ESC */
		return procESC(term, head + 1, tail);

	default: /* 未対応 */
		fprintf(stderr, "Not Supported C0: (%#x)\n", *head);
	}

	return head + 1;
}

const char *
procESC(Term *term, const char *head, const char *tail)
{
	if (head >= tail)
		return NULL;

	switch (*head) {
	case 0x5b: /* CSI */
		return procCSI(term, head + 1, tail);

	case 0x58: /* SOS */
		return procSOS(term, head + 1, tail);

	case 0x50: /* DCS */
	case 0x5d: /* OSC */
	case 0x5e: /* PM */
	case 0x5f: /* APC */
		return procCStr(term, head, tail);

	default:
		/*
		 * 未対応
		 *
		 * 0x20-0x2f   nF型
		 * 0x30-0x3f   Fp/3Fp型 私用制御機能
		 * 0x40-0x5f   Fe型     C1 補助集合
		 * 0x60-0x7e   Fs型     標準単独制御機能 (standardのs?)
		 */
		if (BETWEEN(*head, 0x20, 0x7f))
			fprintf(stderr, "Not Supported ESC Seq: ESC %c\n", *head);
		else
			fprintf(stderr, "Invalid ESC Seq: ESC(%#x)\n", *head);
	}

	return head + 1;
}

const char *
procCSI(Term *term, const char *head, const char *tail)
{
	char param[tail - head + 1], *pp;
	char inter[tail - head + 1], *pi;

	/* パラメタバイト */
	for (pp = param; BETWEEN(*head, 0x30, 0x40); head++) {
		if (head >= tail)
			return NULL;
		*pp++ = *head;
	}
	*pp = '\0';

	/* 中間バイト */
	for (pi = inter; BETWEEN(*head, 0x20, 0x30); head++) {
		if (head >= tail)
			return NULL;
		*pi++ = *head;
	}
	*pi = '\0';

	if (head >= tail)
		return NULL;

	/* 終端バイト */
	switch (*head) {
	default: /* 未対応 */
		if (BETWEEN(*head, 0x40, 0x7f))
			fprintf(stderr, "Not Supported CSI: CSI(%s)%s%c\n",
					param, inter, *head);
		else
			fprintf(stderr, "Invalid CSI: CSI(%s)%s(%#x)\n",
					param, inter, *head);
	}

	return head + 1;
}

const char *
procSOS(Term *term, const char *head, const char *tail)
{
	const char *ps, *ps2;

	/* ST (ESC \) が出てくるまで継続 */
	for (ps = head; !(*(ps - 1) == 0x1b && *ps == 0x5c); ps++) {
		if (ps >= tail)
			return NULL;
	}

	/* 未対応 */
	fprintf(stderr, "Not Supported SOS: SOS ");
	for (ps2 = head; ps2 <= ps; ps2++) {
		if (BETWEEN(*ps2, 0x20, 0x7f))
			fprintf(stderr, "%c", *ps2);
		else
			fprintf(stderr, "(%#x)", *ps2);
	}
	fprintf(stderr, "\n");

	return ps + 1;
}

const char *
procCStr(Term *term, const char *head, const char *tail)
{
	const char *ps, *ps2;

	/* ST (ESC \) が出てくるまで継続 */
	for (ps = head; !(*(ps - 1) == 0x1b && *ps == 0x5c); ps++) {
		if (ps >= tail)
			return NULL;

		/* 指令列に使えない文字が出たら終了 */
		if (!BETWEEN(*ps, 0x20, 0x7f))
			break;
	}

	/* 未対応 */
	fprintf(stderr, "Not Supported CtrlStr: ESC ");
	for (ps2 = head; ps2 <= ps; ps2++) {
		if (BETWEEN(*ps2, 0x20, 0x7f))
			fprintf(stderr, "%c", *ps2);
		else
			fprintf(stderr, "(%#x)", *ps2);
	}
	fprintf(stderr, "\n");

	return ps + 1;
}

ssize_t
writePty(Term *term, const char *buf, ssize_t n)
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
