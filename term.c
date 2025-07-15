#include <sys/ioctl.h>
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

#define READ_SIZE       1024
#define LINE(a,b)       (a->lines[b % a->maxlines])

static void setDefaultPalette(Term *);
static const char *procNCCs(Term *, const char *);
static const char *procCC(Term *, const char *, const char *);
static const char *procESC(Term *, const char *, const char *);
static const char *procCSI(Term *, const char *, const char *);
static const char *procCStr(Term *, const char *, const char *);
static const char *procSOS(Term *, const char *, const char *);
static void areaScroll(Term *, int, int, int);
static void optset(Term *, unsigned int, int);
static void decset(Term *, unsigned int, int);
static void setScrBufSize(Term *term, int, int);
static void setSGR(Term *, char *);

Term *
openTerm(void)
{
	Term *term;
	char *sname;
	int slave;
	int i;

	/* 構造体の初期化 */
	term = xmalloc(sizeof(Term));
	*term = (Term){
		.master = -1,
		.ori = {}, .alt = {}, .sb = NULL,
		.cx = 0, .cy = 0, .svx = 0, .svy = 0,
		.readbuf = NULL,
		.rblen = 0,
		.opt = {0}, .dec = {0},
		.attr = 0, .fg = deffg, .bg = defbg,
		.palette = NULL
	};

	/* スクリーンバッファの初期化 */
	term->ori = term->alt = (struct ScreenBuffer){
		.lastline = 24,
		.maxlines = 256,
		.rows = 24, .cols = 80,
		.scrs = 0, .scre = 23,
	};
	term->ori.lines = xmalloc(term->ori.maxlines * sizeof(Line *));
	term->alt.lines = xmalloc(term->alt.maxlines * sizeof(Line *));
	term->sb = &term->ori;
	for (i = 0; i < term->ori.maxlines; i++)
		term->ori.lines[i] = allocLine();
	for (i = 0; i < term->alt.maxlines; i++)
		term->alt.lines[i] = allocLine();

	/* リードバッファの初期化 */
	term->readbuf = xmalloc(1);
	term->readbuf[0] = '\0';

	/* オプション*/
	term->dec[25 / 8] |=  1 << (25 % 8);

	/* カラーパレットの初期化 */
	term->palette = xmalloc(MAX(256, MAX(deffg, defbg) + 1) * sizeof(Color));
	setDefaultPalette(term);

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
		close(slave);
		setenv("TERM", "st-256color", 1);
		if (setsid() < 0)
			fatal("setsid failed.\n");
		if (execlp("bash", "bash", NULL) < 0)
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
setDefaultPalette(Term *term)
{
	const int vals[6] = {0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff};
	int i;

	/* Selenized Black (https://github.com/jan-warchol/selenized) */
	term->palette[  0] = 0x3b3b3b;
	term->palette[  1] = 0xed4a46;
	term->palette[  2] = 0x70b433;
	term->palette[  3] = 0xdbb32d;
	term->palette[  4] = 0x368aeb;
	term->palette[  5] = 0xeb6eb7;
	term->palette[  6] = 0x3fc5b7;
	term->palette[  7] = 0xb9b9b9;

	term->palette[  8] = 0x545454;
	term->palette[  9] = 0xff5e56;
	term->palette[ 10] = 0x83c746;
	term->palette[ 11] = 0xefc541;
	term->palette[ 12] = 0x4f9cfe;
	term->palette[ 13] = 0xff81ca;
	term->palette[ 14] = 0x56d8c9;
	term->palette[ 15] = 0xdedede;

	/* 216 colors */
	for (i = 0; i < 216; i++)
		term->palette[i + 16] =
			(vals[(i / 36) % 6] << 16) +
			(vals[(i /  6) % 6] <<  8) +
			(vals[(i /  1) % 6] <<  0);

	/* Grayscale colors */
	for (i = 0; i < 24; i++)
		term->palette[i + 232] = 0x0a0a0a * i + 0x080808;

	/* fg bg */
	term->palette[deffg] = 0xb9b9b9;
	term->palette[defbg] = 0x181818;
}

void
closeTerm(Term *term)
{
	int i;

	if (term == NULL)
		return;

	if (0 <= term->master)
		close(term->master);

	for (i = 0; i < term->sb->maxlines; i++) {
		freeLine(term->ori.lines[i]);
		freeLine(term->alt.lines[i]);
	}

	free(term->ori.lines);
	free(term->alt.lines);
	free(term->readbuf);
	free(term->palette);
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
		rest = procCC(term, reading, tail);
		if (rest == NULL) {
			break;
		} else if (reading != rest) {
			memmove(term->readbuf, rest, tail - rest + 1);
			tail -= rest - term->readbuf;
			reading = term->readbuf;
			continue;
		}

		rest = procNCCs(term, reading);
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

const char *
procNCCs(Term *term, const char *head)
{
	const int len = strlen(head) + 1;
	char32_t decoded[len];
	const char *rest;
	Line *line;

	rest = u8sToU32s(decoded, head, len);
	if ((line = getLine(term, term->cy)))
		term->cx += putU32s(line, term->cx, decoded, term->attr,
				term->fg, term->bg, u32slen(decoded));

	return rest;
}

const char *
procCC(Term *term, const char *head, const char *tail)
{
	struct ScreenBuffer *sb = term->sb;
	Line *line;

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
		if (term->cy < sb->scre) {
			term->cy++;
		} else if (0 < sb->scrs || sb->scre < sb->rows - 1) {
			areaScroll(term, sb->scrs, sb->scre, 1);
		} else {
			sb->lastline++;
			if ((line = getLine(term, term->cy)))
				PUT_NUL(line, 0);
		}
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
	struct ScreenBuffer *sb = term->sb;

	if (head >= tail)
		return NULL;

	switch (*head) {
	/* 文字バンクの切り替え(未実装) */
	case 0x24: /* G1D4 */
		head++;
	case 0x28: /* GZD4 */
	case 0x29: /* G1D4 */
	case 0x2a: /* G2D4 */
	case 0x2b: /* G2D4 */
	case 0x2c: /* GZD6 */
	case 0x2d: /* G1D6 */
	case 0x2e: /* G2D6 */
	case 0x2f: /* G3D6 */
		head++;
		return head < tail ? head + 1 : NULL;

	case 0x4d: /* RI */
		if (sb->scrs < term->cy)
			term->cy--;
		else
			areaScroll(term, sb->scrs, sb->scre, -1);
		break;

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
			fprintf(stderr, "Not Supported ESC Seq: ESC %c(%#x)\n",
					*head, *head);
		else
			fprintf(stderr, "Invalid ESC Seq: ESC (%#x)\n", *head);
	}

	return head + 1;
}

const char *
procCSI(Term *term, const char *head, const char *tail)
{
	struct ScreenBuffer *sb = term->sb;
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
		term->cy = MAX(term->cy - MAX(atoi(param), 1), sb->scrs);
		break;

	case 0x42: /* CUD */
		term->cy = MIN(term->cy + MAX(atoi(param), 1), sb->scre);
		break;

	case 0x43: /* CUF */
		term->cx = MIN(term->cx + MAX(atoi(param), 1), sb->cols - 1);
		break;

	case 0x44: /* CUB */
		term->cx = MAX(term->cx - MAX(atoi(param), 1), 0);
		break;

	case 0x47: /* CHA カーソル文字位置決め */
		term->cx = CLIP(atoi(param), 1, sb->cols) - 1;
		break;

	case 0x48: /* CUP カーソル位置決め */
		str1 = strtok2(param, ";:");
		str2 = strtok2(NULL, ";:");
		term->cy = CLIP(atoi(str1 ? str1 : "1"), 1, sb->rows) - 1;
		term->cx = CLIP(atoi(str2 ? str2 : "1"), 1, sb->cols) - 1;
		break;

	case 0x4a: /* ED ページ内消去 */
		if (!(line = getLine(term, term->cy)))
			break;
		switch (*param) {
		default:
		case '0':
			PUT_NUL(line, term->cx);
			begin = term->cy + 1;
			end = sb->rows;
			break;
		case '1':
			begin = 0;
			end = term->cy;
			for (i = 0; i <= term->cx; i++)
				PUT_SPC(line, i);
			break;
		case '2':
			begin = 0;
			end = sb->rows;
			break;
		}
		for (i = begin; i < end; i++)
			if ((line = getLine(term, i)))
				PUT_NUL(line, 0);
		break;

	case 0x4b: /* EL 行内消去 */
		if (!(line = getLine(term, term->cy)))
			break;
		switch (*param) {
		default:
		case '0':
			PUT_NUL(line, term->cx);
			break;
		case '1':
			for (i = 0; i <= term->cx; i++)
				PUT_SPC(line, i);
			break;
		case '2':
			PUT_NUL(line, 0);
			break;
		}
		break;

	case 0x4c: /* IL 行挿入 */
		areaScroll(term, term->cy, sb->scre, -MAX(atoi(param), 1));
		break;

	case 0x4d: /* DL 行削除 */
		areaScroll(term, term->cy, sb->scre, MAX(atoi(param), 1));
		break;

	case 0x50: /* DHC 文字削除 */
		if (!(line = getLine(term, term->cy)))
			break;
		deleteChars(line, term->cx, MAX(atoi(param), 1));
		break;

	case 0x58: /* ECH 文字消去 */
		if (!(line = getLine(term, term->cy)))
			break;
		for (i = atoi(param); 0 < i; i--)
			PUT_SPC(line, term->cx + i - 1);
		break;


	case 0x64: /* VPA 行位置決め */
		term->cy = CLIP(atoi(param), 1, term->sb->rows) - 1;
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

	case 0x6d: /* SGR 表示様式選択 */
		setSGR(term, strlen(param) ? param : "0");
		break;

	case 0x72: /* DECSTBM スクロール範囲設定 */
		str1 = strtok(param, ";");
		str2 = strtok(NULL, ";");
		if (str1 && str2 && atoi(str1) <= atoi(str2)) {
			sb->scrs = MIN(atoi(str1), sb->rows) - 1;
			sb->scre = MIN(atoi(str2), sb->rows) - 1;
		} else {
			sb->scrs = 0;
			sb->scre = sb->rows - 1;
		}
		break;

	default: /* 未対応 */
		if (BETWEEN(*head, 0x40, 0x7f))
			fprintf(stderr, "Not Supported CSI: CSI [%s][%s]%c(%#x)\n",
					param, inter, *head, *head);
		else
			fprintf(stderr, "Invalid CSI: CSI [%s][%s](%#x)\n",
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
	struct ScreenBuffer *sb = term->sb;
	int area = last - first + 1;
	Line *tmp[area];
	int index;
	int i;

	if (!(0 <= first && first <= last && last < sb->rows))
		return;

	for (i = 0; i < area; i++) {
		index = sb->lastline - (sb->rows - 1) + first + i;
		tmp[i] = LINE(sb, index);
	}

	for (i = 0; i < area; i++) {
		index = sb->lastline - (sb->rows - 1) + first + i;

		if (i - num < 0 || area <= i - num)
			freeLine(LINE(sb, index));

		if (0 <= num + i && num + i < area)
			LINE(sb, index) = tmp[num + i];

		if (i + num < 0 || area <= i + num)
			LINE(sb, index) = allocLine();
	}
}

void
optset(Term *term, unsigned int num, int flag)
{
	if (sizeof(term->opt) * 8 <= num) {
		fprintf(stderr, "Option: %d %s\n", num, flag ? "set" : "rst");
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
	struct ScreenBuffer *oldsb;
	Line *line;
	int i;

	switch (num) {
	case 25:   /* カーソル表示切替 */
	case 1006: /* マウス(SGR) */
		break;

	case 1049:  /* altscreen */
		oldsb = term->sb;
		term->sb = flag ? &term->alt : &term->ori;

		if (oldsb == term->sb)
			break;

		if (flag) {
			term->svx = term->cx;
			term->svy = term->cy;
			for (i = 0; i < term->sb->rows; i++)
				if ((line = getLine(term, i)))
					PUT_NUL(line, 0);
		} else {
			term->cx = term->svx;
			term->cy = term->svy;
		}

		setScrBufSize(term, oldsb->rows, oldsb->cols);
		break;

	default:
		fprintf(stderr, "DEC Option: %d %s\n", num, flag ? "set" : "rst");
		if (sizeof(term->dec) * 8 <= num)
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
	struct ScreenBuffer *sb = term->sb;
	int index = sb->lastline - (sb->rows - 1) + row;
	int oldest = sb->lastline - (sb->maxlines - 1);

	if (index < 0 || index < oldest || sb->lastline < index)
		return NULL;

	return LINE(sb, index);
}

void
setWinSize(Term *term, int row, int col, int xpixel, int ypixel)
{
	struct winsize ws;

	row = MIN(row, term->sb->maxlines);
	row = MAX(row, 1);
	col = MAX(col, 1);
	ws = (struct winsize){row, col, xpixel, ypixel};

	term->cx = MIN(term->cx, col - 1);
	term->sb->cols = col;

	setScrBufSize(term, row, col);

	ioctl(term->master, TIOCSWINSZ, &ws);
}

void
reportMouse(Term *term, int btn, int type, int mx, int my)
{
	char buf[40], finc;
	int len;

	/* 現状はSGRのみ対応 */
	if (!GETOPT(term->dec, 1006))
		return;

	/* MOVEは別のセルに移動したときだけ報告 */
	if (type == MOVE && term->oldmx == mx && term->oldmy == my)
		return;
	term->oldmx = mx;
	term->oldmy = my;

	/* 報告を実行 */
	btn += type == MOVE ? 32 : 0;
	finc = type == RELEASE ? 'm' : 'M';
	len = snprintf(buf, sizeof(buf), "\e[<%d;%d;%d%c", btn, mx, my, finc);
	writePty(term, buf, len);

	/* 確認用の表示 */
	printf("(nomal, hilight, button, any) = (%d,%d,%d,%d)\n",
			GETOPT(term->dec, 1000),
			GETOPT(term->dec, 1001),
			GETOPT(term->dec, 1002),
			GETOPT(term->dec, 1003));
}

void
setScrBufSize(Term *term, int row, int col)
{
	struct ScreenBuffer *sb = term->sb;

	while (sb->rows < row) {
		sb->rows++;
		if (sb->rows - 1 < sb->lastline)
			term->cy++;
		else
			PUT_NUL(LINE(sb, sb->lastline++), 0);
	}
	while (row < sb->rows) {
		sb->rows--;
		if (sb->rows - 1 < term->cy ||
				LINE(sb, sb->lastline)->str[0] != L'\0')
			term->cy = MAX(term->cy - 1, 0);
		else
			sb->lastline--;
	}
	sb->cols = col;
	sb->scrs = 0;
	sb->scre = row - 1;
}

void
setSGR(Term *term, char *param)
{
	char *buf;
	int n;

	for (buf = strtok(param, ";"); buf; buf = strtok(NULL, ";")) {
		n = atoi(buf);

		/* すべての効果を取り消す */
		if (n == 0) {
			term->attr = 0;
			term->fg = deffg;
			term->bg = defbg;
		}

		/* 属性 */
		if (BETWEEN(n, 1, 10))
			term->attr |= 1 << (n - 1);
		if (n == 21)
			term->attr |= DULINE;
		if (n == 22)
			term->attr &= ~(BOLD | FAINT);
		if (n == 23)
			term->attr &= ~ITALIC;
		if (n == 24)
			term->attr &= ~(ULINE | DULINE);
		if (n == 25)
			term->attr &= ~(BLINK | RAPID);
		if (n == 27)
			term->attr &= ~NEGA;
		if (n == 28)
			term->attr &= ~CONCEAL;
		if (n == 29)
			term->attr &= ~STRIKE;

		/* フォント*/
		if (BETWEEN(n, 10, 21))
			fprintf(stderr, "font:%d\n", n - 10);

		/* 文字色 */
		if (BETWEEN(n, 30, 38))
			term->fg = n - 30;
		if (BETWEEN(n, 90, 98))
			term->fg = n - 82;
		if (n == 38) {
			if (atoi(strtok(NULL, ";")) == 5)
				term->fg = atoi(strtok(NULL, ";"));
			else
				fprintf(stderr, "Not Supported SGR: %s\n", param);
		}
		if (n == 39)
			term->fg = deffg;

		/* 背景色 */
		if (BETWEEN(n, 40, 48))
			term->bg = n - 40;
		if (BETWEEN(n, 100, 108))
			term->bg = n - 92;
		if (n == 48) {
			if (atoi(strtok(NULL, ";")) == 5)
				term->bg = atoi(strtok(NULL, ";"));
			else
				fprintf(stderr, "Not Supported SGR: %s\n", param);
		}
		if (n == 49)
			term->bg = defbg;

		/* その他の効果 */
		if (BETWEEN(n, 51, 70) && n != 65)
			fprintf(stderr, "effect:%d\n", n);
		if (n == 65)
			fprintf(stderr, "cancel effect: %d\n", n);
	}
}
