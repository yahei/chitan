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

#define READ_SIZE       4096
#define LINE(a,b)       (a->lines[b % a->maxlines])

static void setDefaultPalette(Term *);
static const char *procNCCs(Term *, const char *);
static const char *procCC(Term *, const char *, const char *);
static const char *procESC(Term *, const char *, const char *);
static const char *procCSI(Term *, const char *, const char *);
static const char *procSOS(Term *, const char *, const char *);
static const char *procOSC(Term *, const char *, const char *);
static const char *procCStr(Term *, const char *, const char *);
static void linefeed(Term *);
static void setCursorPos(Term *, int, int);
static void moveCursorPos(Term *, int, int);
static void areaScroll(Term *, int, int, int);
static void optset(Term *, unsigned int, int);
static void decset(Term *, unsigned int, int);
static void setScrBufSize(Term *term, int, int);
static void setSGR(Term *, char *);
static const char *designateCharSet(Term *, const char *, const char *);

Term *
openTerm(void)
{
	Term *term;
	char *sname;
	int slave;
	int i;

	/* 構造体の初期化 */
	term = xmalloc(sizeof(Term));
	*term = (Term){ .master = -1, .ctype = 2, .fg = deffg, .bg = defbg };

	/* スクリーンバッファの初期化 */
	term->ori = term->alt = (struct ScreenBuffer){
		.firstline = 0,
		.totallines = 24,
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

	/* オプションの初期化 */
	memset(term->opt, 1, 64);
	memset(term->dec, 1, 8800);

	/* カラーパレットの初期化 */
	term->palette = xmalloc(MAX(256, MAX(deffg, defbg) + 1) * sizeof(Color));
	setDefaultPalette(term);

	/* 疑似端末を開く */
	errno = -1;
	if ((term->master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK)) < 0)
		goto FAIL;
	if ((sname = ptsname(term->master)) == NULL)
		goto FAIL;
	if (grantpt(term->master) < 0)
		goto FAIL;
	if (unlockpt(term->master) < 0)
		goto FAIL;
	if ((slave = open(sname, O_RDWR | O_NOCTTY)) < 0)
		goto FAIL;

	/* slave側でプロセスを起動 */
	switch (fork()) {
	case -1:/* 失敗 */
		goto FAIL;
		break;

	case 0: /* slave側 */
		close(term->master);
		dup2(slave, 0);
		dup2(slave, 1);
		dup2(slave, 2);
		close(slave);
		setenv("TERM", "chitan-256color", 1);
		if (setsid() < 0)
			fatal("setsid failed.\n");
		if (ioctl(0, TIOCSCTTY) < 0)
			fprintf(stderr, "TIOCSCTTY failed.\n");
		if (execlp("bash", "bash", NULL) < 0)
			fatal("exec failed.\n");
		break;

	default: /* master側 */
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
	char32_t decoded[len], *dp;
	const char *rest;
	Line *line;
	int max = term->sb->cols - term->cx, wlen;
	int i;

	/* UTF32に変換 */
	rest = u8sToU32s(decoded, head, len);

	/* 図形文字集合が切り替えられていたら文字を置き換える */
	if (term->g[0])
		for (i = 0; i < len; i++)
			if (BETWEEN(decoded[i], 0x21, 0x7f))
				decoded[i] = term->g[0][decoded[i] - 0x21];

	for (dp = decoded; *dp != L'\0'; dp += MAX(0, wlen)) {
		/* 自動改行 */
		if (term->sb->am) {
			term->sb->am = 0;
			max = term->sb->cols;
			setCursorPos(term, 0, term->cy);
			linefeed(term);
		}

		/* 行が埋まる場合は自動改行を設定して行末までを書く */
		term->sb->am = 0 < term->dec[7] && max < u32swidth(dp, u32slen(dp));
		wlen = term->sb->am ? getCharCnt(dp, max).index : u32slen(dp);
		wlen = MAX(wlen, 1);

		/* 書き込む */
		if ((line = getLine(term, term->cy)))
			term->cx += putU32s(line, term->cx, dp, term->attr,
					term->fg, term->bg, wlen);
	}

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
		if (term->bell)
			term->bell(term->bellp);
		break;

	case 0x08: /* BS */
		moveCursorPos(term, -1, 0);
		break;

	case 0x09: /* HT */
		moveCursorPos(term, 8 - term->cx % 8, 0);
		break;

	case 0x0a: /* LF */
		linefeed(term);
		break;

	case 0x0d: /* CR */
		setCursorPos(term, 0, term->cy);
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

	if (tail <= head)
		return NULL;

	switch (*head) {
	case 0x24: /* DESIGGNAE MULTIBYTE */
	case 0x28: /* G0-DESIGGNAE 94-SET */
	case 0x29: /* G1-DESIGGNAE 94-SET */
	case 0x2a: /* G2-DESIGGNAE 94-SET */
	case 0x2b: /* G3-DESIGGNAE 94-SET */
	case 0x2c: /* G0-DESIGGNAE 96-SET (使用しない) */
	case 0x2d: /* G1-DESIGGNAE 96-SET */
	case 0x2e: /* G2-DESIGGNAE 96-SET */
	case 0x2f: /* G3-DESIGGNAE 96-SET */
		return designateCharSet(term, head, tail);

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

	case 0x5d: /* OSC */
		return procOSC(term, head + 1, tail);

	case 0x50: /* DCS */
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
	int i, len, begin, end;

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
	case 0x40: /* ICH 文字挿入 */
		if ((line = getLine(term, term->cy))) {
			int n = MAX(atoi(param), 1);
			char32_t str[n];
			int attr[n], fg[n], bg[n];
			INIT(str, L' ');
			INIT(attr, NONE);
			INIT(fg, deffg);
			INIT(bg, defbg);
			InsertLine il = { str, attr, fg, bg };
			insertU32s(line, getCharCnt(line->str, term->cx).index, &il, n);
		}
		break;

	case 0x41: /* CUU */
		moveCursorPos(term, 0, -MAX(atoi(param), 1));
		break;

	case 0x42: /* CUD */
		moveCursorPos(term, 0, MAX(atoi(param), 1));
		break;

	case 0x43: /* CUF */
		moveCursorPos(term, MAX(atoi(param), 1), 0);
		break;

	case 0x44: /* CUB */
		moveCursorPos(term, -MAX(atoi(param), 1), 0);
		break;

	case 0x47: /* CHA カーソル文字位置決め */
		setCursorPos(term, atoi(param) - 1, term->cy);
		break;

	case 0x48: /* CUP カーソル位置決め */
		str1 = strtok2(param, ";:");
		str2 = strtok2(NULL, ";:");
		setCursorPos(term, atoi(str2 ? str2 : "1") - 1,
		                   atoi(str1 ? str1 : "1") - 1);
		break;

	case 0x4a: /* ED ページ内消去 */
		if (!(line = getLine(term, term->cy)))
			break;
		switch (*param) {
		default:
		case '0':
			if (0 < (len = term->sb->cols - term->cx))
				putSPCs(line, term->cx, term->bg, len);
			begin = term->cy + 1;
			end = sb->rows;
			break;
		case '1':
			begin = 0;
			end = term->cy;
			putSPCs(line, 0, term->bg, term->cx + 1);
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
			if (0 < (len = term->sb->cols - term->cx))
				putSPCs(line, term->cx, term->bg, len);
			break;
		case '1':
			putSPCs(line, 0, term->bg, term->cx + 1);
			break;
		case '2':
			putSPCs(line, 0, term->bg, term->sb->cols);
			break;
		}
		break;

	case 0x4c: /* IL 行挿入 */
		areaScroll(term, term->cy, sb->scre, -MAX(atoi(param), 1));
		break;

	case 0x4d: /* DL 行削除 */
		areaScroll(term, term->cy, sb->scre, MAX(atoi(param), 1));
		break;

	case 0x45: /* CNL カーソル復帰行前進 */
		moveCursorPos(term, 0, MAX(atoi(param), 1));
		setCursorPos(term, 0, term->cy);
		break;

	case 0x46: /* CPL カーソル復帰行後退 */
		moveCursorPos(term, 0, -MAX(atoi(param), 1));
		setCursorPos(term, 0, term->cy);
		break;

	case 0x50: /* DHC 文字削除 */
		if (!(line = getLine(term, term->cy)))
			break;
		eraseInLine(line, term->cx, MAX(atoi(param), 1));
		break;

	case 0x53: /* SU スクロール上 */
		areaScroll(term, sb->scrs, sb->scre, MAX(atoi(param), 1));
		break;

	case 0x54: /* SD スクロール下 */
		areaScroll(term, sb->scrs, sb->scre, -MAX(atoi(param), 1));
		break;

	case 0x58: /* ECH 文字消去 */
		if (!(line = getLine(term, term->cy)))
			break;
		putSPCs(line, term->cx, term->bg, atoi(param));
		break;


	case 0x64: /* VPA 行位置決め */
		setCursorPos(term, term->cx, atoi(param) - 1);
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

	case 0x71: /* DECSCUSR カーソル形状設定 */
		if (*inter == ' ')
			term->ctype = atoi(param);
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
	const char *p;
	char *perr, err[tail - head + 1];

	for (p = head, perr = err;; p++, perr++) {
		/* 正常終了 */
		if (p < tail && p[0] == 0x1b && p[1] == 0x5c) {
			p++;
			break;
		}

		/* 中断 */
		if (p < tail && p[0] == 0x1b && p[1] == 0x58)
			return p;
		if (tail <= p)
			return NULL;

		/* 未対応の表示用 */
		*perr = BETWEEN(*p, 0x20, 0x7f) ? *p : '?';
	}
	err[p - head] = '\0';

	/* 未対応 */
	fprintf(stderr, "Not Supported SOS: %s\n", err);
	return p + 1;
}

const char *
procOSC(Term *term, const char *head, const char *tail)
{
	const char *p;
	char *perr, err[tail - head + 1];

	for (p = head, perr = err;; p++, perr++) {
		/* 正常終了 */
		if (p[0] == 0x07)
			break;
		if (p < tail && p[0] == 0x1b && p[1] == 0x5c) {
			p++;
			break;
		}

		/* 中断 */
		if (!(BETWEEN(*p, 0x08, 0x0e) || BETWEEN(*p, 0x20, 0x7f)))
			return p;
		if (tail <= p)
			return NULL;

		/* 未対応の表示用 */
		*perr = BETWEEN(*p, 0x20, 0x7f) ? *p : '?';
	}
	err[p - head] = '\0';

	/* 未対応 */
	fprintf(stderr, "Not Supported OSC: %s\n", err);
	return p + 1;
}

const char *
procCStr(Term *term, const char *head, const char *tail)
{
	const char *p;
	char *perr, err[tail - head + 1];

	for (p = head, perr = err;; p++, perr++) {
		/* 正常終了 */
		if (p < tail && p[0] == 0x1b && p[1] == 0x5c) {
			p++;
			break;
		}

		/* 中断 */
		if (!(BETWEEN(*p, 0x08, 0x0e) || BETWEEN(*p, 0x20, 0x7f)))
			return p;
		if (tail <= p)
			return NULL;

		/* 未対応の表示用 */
		*perr = BETWEEN(*p, 0x20, 0x7f) ? *p : '?';
	}
	err[p - head] = '\0';

	/* 未対応 */
	fprintf(stderr, "Not Supported CtrlStr: %s\n", err);
	return p + 1;
}

void
linefeed(Term *term)
{
	struct ScreenBuffer *sb = term->sb;
	Line *line;

	if (term->cy < sb->scre) {
		term->cy++;
	} else if (0 < sb->scrs || sb->scre < sb->rows - 1) {
		areaScroll(term, sb->scrs, sb->scre, 1);
	} else {
		sb->firstline++;
		if (sb->totallines < sb->firstline + sb->rows)
			sb->totallines++;
		if ((line = getLine(term, term->cy)))
			PUT_NUL(line, 0);
	}

	term->sb->am = 0;
}

void
setCursorPos(Term *term, int cx, int cy)
{
	term->cx = CLIP(cx, 0, term->sb->cols - 1);
	term->cy = CLIP(cy, term->sb->scrs, term->sb->scre);
}

void
moveCursorPos(Term *term, int dx, int dy)
{
	setCursorPos(term, term->cx + dx, term->cy + dy);
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
		index = sb->totallines - sb->rows + first + i;
		tmp[i] = LINE(sb, index);
	}

	for (i = 0; i < area; i++) {
		index = sb->totallines - sb->rows + first + i;

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
	if (sizeof(term->opt) <= num) {
		fprintf(stderr, "Option: %d %s\e[m\n", num,
				flag ? "\e[32mset" : "\e[31mrst");
		return;
	}

	term->opt[num] = flag ? 2 : 0;
}

void
decset(Term *term, unsigned int num, int flag)
{
	struct ScreenBuffer *oldsb;
	Line *line;
	int i;

	switch (num) {
	case 25:   /* カーソル表示切替 */
	case 9:    /* マウス X10 */
	case 1000: /* マウス normal */
	case 1002: /* マウス button */
	case 1003: /* マウス any */
	case 1006: /* マウス SGR */
	case 2004: /* Bracketed Paste Mode */
		break;

	case 7:    /* 自動改行モード */
		term->sb->am = 0;
		break;

	case 1049:  /* altscreen clear */
	case 1047:  /* altscreen */
		oldsb = term->sb;
		term->sb = flag ? &term->alt : &term->ori;

		if (oldsb == term->sb)
			break;

		if (flag) {
			term->svx = term->cx;
			term->svy = term->cy;
			if (num == 1049)
				for (i = 0; i < term->sb->rows; i++)
					if ((line = getLine(term, i)))
						PUT_NUL(line, 0);
		} else {
			setCursorPos(term, term->svx, term->svy);
		}

		setScrBufSize(term, oldsb->rows, oldsb->cols);
		break;

	default:
		fprintf(stderr, "DEC Option: %d %s\e[m\n", num,
				flag ? "\e[32mset" : "\e[31mrst");
		if (sizeof(term->dec) <= num)
			return;
	}

	term->dec[num] = flag ? 2 : 0;
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
	int index = sb->firstline + row;
	int oldest = MAX(sb->totallines - sb->maxlines, 0);

	if (index < oldest || sb->totallines <= index || sb->rows <= row)
		return NULL;

	return LINE(sb, index);
}

void
setWinSize(Term *term, int row, int col, int xpixel, int ypixel)
{
	struct winsize ws;

	row = CLIP(row, 1, term->sb->maxlines);
	col = MAX(col, 2);
	ws = (struct winsize){ row, col, xpixel, ypixel };

	if (term->sb->rows != row || term->sb->cols != col)
		setScrBufSize(term, row, col);
	ioctl(term->master, TIOCSWINSZ, &ws);
}

void
setScrBufSize(Term *term, int row, int col)
{
	struct ScreenBuffer *sb = term->sb;
	int newfst = sb->firstline;

	/* 行数が減ってカーソルが画面外に出たとき */
	if (row < sb->rows && row - 1 < term->cy) {
		newfst = sb->firstline + (sb->rows - row);
		newfst = MIN(sb->firstline + term->cy, newfst);
	}
	/* 行数が増えたとき */
	if (sb->rows < row) {
		newfst = MAX(sb->firstline - (row - sb->rows), 0);
		newfst = MAX(sb->totallines - sb->maxlines, newfst);
		sb->totallines = MAX(newfst + row, sb->totallines);
	}

	/* 画面サイズ変更 */
	sb->rows = row;
	sb->cols = col;
	sb->scrs = 0;
	sb->scre = row - 1;

	/* firstlineの変更とカーソル移動を実行 */
	if (sb->firstline != newfst) {
		moveCursorPos(term, 0, sb->firstline - newfst);
		sb->firstline = newfst;
	}
}

void
reportMouse(Term *term, int btn, int release, int mx, int my)
{
	char buf[40];
	int len = 0;
	enum { normal, button, any } type;

	/* ホイールのreleaseは報告しない */
	if ((btn & WHEEL) && release)
		return;

	/* MOVEは別のセルに移動したときだけ報告 */
	if (btn & MOVE && term->oldmx == mx && term->oldmy == my)
		return;
	term->oldmx = mx;
	term->oldmy = my;

	/* 対象外のイベントは報告しない */
	type = (btn & MOVE) ?  ((btn & 3) == 3) ?  any : button : normal;
	if (!(1 < term->dec[1000] && type <= normal) &&
	    !(1 < term->dec[1002] && type <= button) &&
	    !(1 < term->dec[1003] && type <= any   ))
		return;

	/* 報告を実行 */
	if (1 < term->dec[1006]) {
		/* SGR */
		len = snprintf(buf, sizeof(buf), "\e[<%d;%d;%d%c",
				btn, mx + 1, my + 1, release ? 'm' : 'M');
	} else if (1 <= term->dec[9]) {
		/* X10 */
		if (!BETWEEN(btn, 0, 255 - 32) ||
		    !BETWEEN( mx, 0, 255 - 32) ||
		    !BETWEEN( my, 0, 255 - 32))
			return;
		len = snprintf(buf, sizeof(buf), "\e[M%c%c%c",
				(release ? 3 : btn) + 32, mx + 33, my + 33);
	}
	if (0 < len)
		writePty(term, buf, len);
}

void
setSGR(Term *term, char *param)
{
	char *buf, *buf2;
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

		/* フォント */
		if (BETWEEN(n, 10, 21))
			fprintf(stderr, "font:%d\n", n - 10);

		/* 文字色 */
		if (BETWEEN(n, 30, 38))
			term->fg = n - 30;
		if (BETWEEN(n, 90, 98))
			term->fg = n - 82;
		if (n == 38) {
			buf = strtok(NULL, ";");
			buf2 = strtok(NULL, ";");
			if (buf && buf2 && atoi(buf) == 5)
				term->fg = atoi(buf2);
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
			buf = strtok(NULL, ";");
			buf2 = strtok(NULL, ";");
			if (buf && buf2 && atoi(buf) == 5)
				term->bg = atoi(buf2);
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

const char *
designateCharSet(Term *term, const char *head, const char *tail)
{
	/* 94文字集合 */
	static const char32_t * const cset94[256] = {
		/* DEC Special Graphics Characters */
		['0'] = (char32_t *)
			L"!\"#$%&'()*+,-./"
			L"0123456789:;<=>?"
			L"@ABCDEFGHIJKLMNO"
			L"PQRSTUVWXYZ[\\]^ "
			L"⧫▒␉␌␍␊°±␤␋┘┐┌└┼⎺"
			L"⎻─⎼⎽├┤┴┬│≤≥π≠£·",
	};
	char *size[] = {"94", "96", "94x94", "96x96"};
	int multi = 0, gnum, set96;
	const char *name;

	/* 複数バイトかどうか */
	multi = (head[0] == 0x24) ;
	if (multi && tail <= ++head)
		return NULL;

	/* バンク番号と94文字集合か96文字集合か */
	if (BETWEEN(head[0], 0x28, 0x30)) {
		gnum = head[0] - 0x28;
		set96 = (0x2c <= gnum);
		gnum %= 4;
		if (tail <= ++head)
			return NULL;
	} else if (BETWEEN(head[0], 0x40, 0x43)) {
		gnum = 0;
		set96 = 0;
	} else {
		return head;
	}

	/* 図形文字集合名 */
	name = head;
	if (strchr("\"%`&", name[0]))
		if (tail <= ++head)
			return NULL;

	/* 設定する */
	term->g[gnum] = NULL;

	if (!multi && !set96 && !strchr("\"%`&", name[0]))
		term->g[gnum] = cset94[(unsigned int)name[0]];

	if (!term->g[gnum] && name[0] != 'B')
		fprintf(stderr, "Not Supported CharSet. (%s %c%c)\n",
				size[multi + set96 * 2], name[0],
				strchr("\"%`&", name[0]) ? name[1] : '\0');

	return head + 1;
}
