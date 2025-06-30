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

#define READ_SIZE       16
#define LINE(a,b)       (a->lines[b % a->maxlines])

static const char *procNCCs(Term *, const char *);
static const char *procCC(Term *, const char *, const char *);
static const char *procESC(Term *, const char *, const char *);
static const char *procCSI(Term *, const char *, const char *);
static const char *procCStr(Term *, const char *, const char *);
static const char *procSOS(Term *, const char *, const char *);
void areaScroll(Term *, int, int, int);
void optset(Term *, unsigned int, int);
void decset(Term *, unsigned int, int);

Term *
openTerm(void)
{
	Term *term;
	char *sname;
	int slave;
	int i;

	/* 構造体の初期化 */
	term = xmalloc(sizeof(Term));
	*term = (Term){ -1, NULL, 32, 24, 0, 0, 24, NULL, 0, {0}, {0}, 0, 23};

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
		setenv("TERM", "st-256color", 1);
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
	term->cx += putU32(getLine(term, term->cy), term->cx,
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
	case 0x00: /* NUL */
		break;

	case 0x07: /* BEL */
		fprintf(stdout, "<BELL>\n");
		break;

	case 0x08: /* BS */
		term->cx -= 1;
		break;

	case 0x09: /* HT */
		term->cx += 8 - term->cx % 8;
		break;

	case 0x0a: /* LF */
		if (term->cy < term->scre) {
			term->cy++;
			break;
		}
		if (0 < term->scrs || term->scre < term->rows - 1) {
			areaScroll(term, term->scrs, term->scre, 1);
			break;
		}
		term->lastline++;
		putU32(getLine(term, term->cy), 0, (char32_t *)L"\0", 1);
		break;

	case 0x0d: /* CR */
		term->cx = 0;
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
		 * 0x60-0x7e   Fs型     標準単独制御機能
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
	char *str1, *str2;
	Line *line;
	int i, begin, end;

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
	case 0x41: /* CUU */
		term->cy = MAX(term->cy - atoi(param), term->scrs);
		break;

	case 0x42: /* CUD */
		term->cy = MIN(term->cy + atoi(param), term->scre);
		break;

	case 0x43: /* CUF */
		term->cx = MIN(term->cx + atoi(param), 80);
		break;

	case 0x44: /* CUB */
		term->cx = MAX(term->cx - atoi(param), 0);
		break;

	case 0x48: /* CUP カーソル位置決め */
		str1 = strtok(param, ";");
		str2 = strtok(NULL, ";");
		if (str1 && str2) {
			term->cy = atoi(str1) - 1;
			term->cx = atoi(str2) - 1;
		}
		break;

	case 0x4a: /* ED ページ内消去 */
		line = getLine(term, term->cy);
		switch (*param) {
		default:
		case '0':
			putU32(line, term->cx, (char32_t *)L"\0", 1);
			begin = term->cy + 1;
			end = term->rows;
			break;
		case '1':
			begin = 0;
			end = term->cy;
			for (i = 0; i <= term->cx; i++)
				putU32(line, i, (char32_t *)L" ", 1);
			break;
		case '2':
			begin = 0;
			end = term->rows;
			break;
		}
		for (i = begin; i < end; i++)
			putU32(getLine(term, i), 0, (char32_t *)L"\0", 1);
		break;

	case 0x4b: /* EL 行内消去 */
		line = getLine(term, term->cy);
		switch (*param) {
		default:
		case '0':
			putU32(line, term->cx, (char32_t *)L"\0", 1);
			break;
		case '1':
			for (i = 0; i <= term->cx; i++)
				putU32(line, i, (char32_t *)L" ", 1);
			break;
		case '2':
			putU32(line, 0, (char32_t *)L"\0", 1);
			break;
		}
		break;

	case 0x4c: /* IL 行挿入 */
		areaScroll(term, term->cy, term->scre, -MAX(atoi(param), 1));
		break;

	case 0x4d: /* DL 行削除 */
		areaScroll(term, term->cy, term->scre, MAX(atoi(param), 1));
		break;

	case 0x68: /* SM DECSET オプション設定 */
		if (*param == '?')
			decset(term, atoi(param + 1), 1);
		else
			optset(term, atoi(param), 1);
		break;

	case 0x6c: /* RM DECRST オプション解除 */
		if (*param == '?')
			decset(term, atoi(param + 1), 0);
		else
			optset(term, atoi(param), 0);
		break;

	case 0x72: /* DECSTBM スクロール範囲設定 */
		str1 = strtok(param, ";");
		str2 = strtok(NULL, ";");
		if (str1 && str2 && atoi(str1) <= atoi(str2)) {
			term->scrs = atoi(str1) - 1;
			term->scre = atoi(str2) - 1;
		} else {
			term->scrs = 0;
			term->scre = term->rows - 1;
		}
		break;

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
		if (!BETWEEN(*ps, 0x08, 0x0e) && !BETWEEN(*ps, 0x20, 0x7f)
				&& *ps != 0x1b)
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

void
areaScroll(Term *term, int first, int last, int num)
{
	int area = last - first + 1;
	Line *tmp[area];
	int index;
	int i;

	for (i = 0; i < area; i++) {
		index = term->lastline - (term->rows - 1) + first + i;
		tmp[i] = LINE(term, index);
	}

	for (i = 0; i < area; i++) {
		index = term->lastline - (term->rows - 1) + first + i;

		if (i - num < 0 || area <= i - num)
			freeLine(LINE(term, index));

		if (0 <= num + i && num + i < area)
			LINE(term, index) = tmp[num + i];

		if (i + num < 0 || area <= i + num)
			LINE(term, index) = allocLine();
	}
}

void
optset(Term *term, unsigned int num, int flag)
{
	if (sizeof(term->opt) * 8 <= num) {
		fprintf(stderr, "Option: %d\n", num);
		return;
	}

	if (flag)
		term->opt[num / 8] |=  1 << (num % 8);
	else
		term->opt[num / 8] &= ~1 << (num % 8);
}

void
decset(Term *term, unsigned int num, int flag)
{
	if (sizeof(term->dec) * 8 <= num) {
		fprintf(stderr, "DEC Option: %d\n", num);
		return;
	}

	if (flag)
		term->dec[num / 8] |=  1 << (num % 8);
	else
		term->dec[num / 8] &= ~1 << (num % 8);
}

ssize_t
writePty(Term *term, const char *buf, ssize_t n)
{
	ssize_t size;
	size = write(term->master, buf, n);
	return size;
}

Line *
getLine(Term *term, int row)
{
	int index = term->lastline - (term->rows - 1) + row;
	int oldest = term->lastline - (term->maxlines - 1);

	if (index < 0 || index < oldest || term->lastline < index)
		return NULL;

	return LINE(term, index);
}

