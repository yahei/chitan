#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include "term.h"
#include "util.h"

/*
 * Term
 *
 * 疑似端末とバッファを管理する
 */

#define READ_SIZE       (1 << 14)
#define LINE(a, b)      ((a)->lines[(b) % (a)->maxlines])
#define IS_GC(c)        (BETWEEN((c), 0x20, 0x7f) || (c) & 0x80)

enum cseq_type { CS_DCS, CS_SOS, CS_OSC, CS_PM, CS_APC, CS_k };

static void setDefaultPalette(Color *);
static void removeCharFromReadbuf(Term *, const char *);
static const char *GCs(Term *, const char *);
static const char *CC(Term *, const char *, const char *);
static const char *ESC(Term *, const char *, const char *);
static const char *CSI(Term *, const char *, const char *);
static const char *ctrlSeq(Term *, const char *, const char *, enum cseq_type type);
static void CStr(Term *, const char *, const char *, const char *);
static void OSC(Term *, char *, const char *);
static void linefeed(Term *);
static void setCursorPos(Term *, int, int);
static void moveCursorPos(Term *, int, int);
static void areaScroll(Term *, int, int, int);
static void optset(Term *, unsigned int, int);
static void decset(Term *, unsigned int, int);
static void setScrBufSize(Term *term, int, int);
static void setSGR(Term *, const char *);
static const char *designateCharSet(Term *, const char *, const char *);

Term *
openTerm(int row, int col, int bufsize, const char *program, char *const cmd[])
{
	Term *term;
	char *sname;
	int slave;
	int i;

	/* 構造体の初期化 */
	term = xmalloc(sizeof(Term));
	*term = (Term){ .master = -1, .ctype = 2, .fg = deffg, .bg = defbg,
		.title = "chitan" };

	/* スクリーンバッファの初期化 */
	row = row < bufsize ? row : bufsize;
	term->ori = term->alt = (struct ScrBuf){
		.firstline = 0,
		.totallines = row,
		.maxlines = bufsize,
		.rows = row, .cols = col,
		.scrs = 0, .scre = row - 1,
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
	term->appkeypad = 1;

	/* カラーパレットの初期化 */
	term->palette     = xmalloc(MAX(256, MAX(deffg, defbg) + 1) * sizeof(Color));
	term->def_palette = xmalloc(MAX(256, MAX(deffg, defbg) + 1) * sizeof(Color));
	setDefaultPalette(term->palette);
	setDefaultPalette(term->def_palette);

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
		if (execvp(program, cmd) < 0)
			fatal("exec failed.\n");
		return NULL;

	default: /* master側 */
		close(slave);
		setWinSize(term, row, col, 0, 0);
	}

	return term;

FAIL:
	closeTerm(term);
	return NULL;
}

void
setDefaultPalette(Color *palette)
{
	const unsigned int vals[6] = {0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff};
	int i;

	/* Selenized Black (https://github.com/jan-warchol/selenized) */
	palette[  0] = 0xff252525;
	palette[  1] = 0xffed4a46;
	palette[  2] = 0xff70b433;
	palette[  3] = 0xffdbb32d;
	palette[  4] = 0xff368aeb;
	palette[  5] = 0xffeb6eb7;
	palette[  6] = 0xff3fc5b7;
	palette[  7] = 0xffb9b9b9;

	palette[  8] = 0xff3b3b3b;
	palette[  9] = 0xffff5e56;
	palette[ 10] = 0xff83c746;
	palette[ 11] = 0xffefc541;
	palette[ 12] = 0xff4f9cfe;
	palette[ 13] = 0xffff81ca;
	palette[ 14] = 0xff56d8c9;
	palette[ 15] = 0xffdedede;

	/* 216 colors */
	for (i = 0; i < 216; i++)
		palette[i + 16] = (0xff << 24) +
			(vals[(i / 36) % 6] << 16) +
			(vals[(i /  6) % 6] <<  8) +
			(vals[(i /  1) % 6] <<  0);

	/* Grayscale colors */
	for (i = 0; i < 24; i++)
		palette[i + 232] = 0x0a0a0a * i + 0xff080808;

	/* fg bg */
	palette[deffg] = 0xffb9b9b9;
	palette[defbg] = 0xff181818;
}

void
closeTerm(Term *term)
{
	int i;

	if (term == NULL)
		return;

	if (0 <= term->master)
		close(term->master);

	for (i = 0; i < term->ori.maxlines; i++)
		freeLine(term->ori.lines[i]);
	for (i = 0; i < term->alt.maxlines; i++)
		freeLine(term->alt.lines[i]);

	free(term->ori.lines);
	free(term->alt.lines);
	free(term->readbuf);
	free(term->palette);
	free(term->def_palette);
	free(term);
}

ssize_t
readPty(Term *term)
{
	const char *head, *reading, *rest, *tail;
	ssize_t size;

	term->readbuf = xrealloc(term->readbuf, term->rblen + READ_SIZE + 1);
	size = read(term->master, term->readbuf + term->rblen, READ_SIZE);
	tail = term->readbuf + term->rblen + size;
	*(char *)tail = '\0';

	/* プロセスが終了してる場合など */
	if (size < 0)
		return size;

	for (head = reading = term->readbuf; reading < tail;) {
		rest = IS_GC(*reading) ? GCs(term, reading) : CC(term, reading, tail);

		if (rest == NULL)
			break;
		else if (rest == reading)
			reading++;
		else
			head = reading = rest;
	}
	memmove(term->readbuf, head, tail - head);
	term->rblen = tail - head;

	return size;
}

void
removeCharFromReadbuf(Term *term, const char *target)
{
	memmove(term->readbuf + 1, term->readbuf, target - term->readbuf);
}

const char *
GCs(Term *term, const char *head)
{
	const int len = strlen(head) + 1;
	char32_t decoded[len], *dp;
	const char *rest;
	Line *line;
	CharCnt cc;
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
		term->sb->am = 0 < term->dec[7] && max <= u32swidth(dp);
		wlen = term->sb->am ? getIndex(dp, max) : u32slen(dp);
		wlen = MAX(wlen, 1);

		/* 書き込む */
		if ((line = getLine(term->sb, term->cy))) {
			term->cx += putU32s(line, term->cx, dp, term->attr,
					term->fg, term->bg, wlen);
			cc = getCharCnt(line->str, term->sb->cols);
			if (cc.index < u32slen(line->str))
				line->str[cc.index] = '\0';
		}
	}

	return rest;
}

const char *
CC(Term *term, const char *head, const char *tail)
{
	/* C0 基本集合 */
	switch (*head) {
	case 0x00:                                              break; /* NUL */
	case 0x07: term->bell_cnt++;                            break; /* BEL */
	case 0x08: moveCursorPos(term, -1, 0);                  break; /* BS  */
	case 0x09: moveCursorPos(term, 8 - term->cx % 8, 0);    break; /* HT  */
	case 0x0a: linefeed(term);                              break; /* LF  */
	case 0x0d: setCursorPos(term, 0, term->cy);             break; /* CR  */
	case 0x1b: return ESC(term, head + 1, tail);                   /* ESC */
	default: fprintf(stderr, "Not Supported C0: (%#x)\n", *head);  /* etc */
	}

	return head + 1;
}

const char *
ESC(Term *term, const char *head, const char *tail)
{
	struct ScrBuf *sb = term->sb;

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

	case 0x3d: /* DECKPAM */
	case 0x3e: /* DECKPNM */
		term->appkeypad = *head == 0x3d ? 2 : 0;
		break;

	case 0x4d: /* RI */
		if (sb->scrs < term->cy)
			term->cy--;
		else if (MAX(sb->totallines - sb->maxlines, 0) < sb->firstline &&
				sb->scrs == 0 && sb->scre == sb->rows - 1)
			sb->firstline--;
		else
			areaScroll(term, sb->scrs, sb->scre, -1);
		break;

	case 0x50: return ctrlSeq(term, head + 1, tail, CS_DCS);    /* DCS */
	case 0x58: return ctrlSeq(term, head + 1, tail, CS_SOS);    /* SOS */
	case 0x5b: return     CSI(term, head + 1, tail);            /* CSI */
	case 0x5d: return ctrlSeq(term, head + 1, tail, CS_OSC);    /* OSC */
	case 0x5e: return ctrlSeq(term, head + 1, tail, CS_PM);     /* PM  */
	case 0x5f: return ctrlSeq(term, head + 1, tail, CS_APC);    /* APC */
	case 0x6b: return ctrlSeq(term, head + 1, tail, CS_k);      /* k   */
	case 0x00: return     ESC(term, head + 1, tail);            /* NUL */

	default:
		/*
		 * 未対応
		 *
		 * 0x20-0x2f   nF型
		 * 0x30-0x3f   Fp/3Fp型 私用制御機能
		 * 0x40-0x5f   Fe型     C1 補助集合
		 * 0x60-0x7e   Fs型     標準単独制御機能
		 */
		if (!BETWEEN(*head, 0x20, 0x7f)) {
			fprintf(stderr, "Invalid ESC Seq: ESC (%#04x)\n", *head);
			return head;
		}
		fprintf(stderr, "Not Supported ESC Seq: ESC %c(%#04x)\n", *head, *head);
	}

	return head + 1;
}

const char *
CSI(Term *term, const char *head, const char *tail)
{
	struct ScrBuf *sb = term->sb;
	char param[tail - head + 1], *pp = param;
	char inter[tail - head + 1];
	char *str1, *str2;
	Line *line = getLine(term->sb, term->cy);
	int i, a, b, len, begin, end, index = 0;

	/* パラメタバイト */
	for (i = 0; BETWEEN(head[index], 0x30, 0x40); param[i++] = head[index++])
		if (head + index >= tail)
			return NULL;
	param[i] = '\0';

	/* 中間バイト */
	for (i = 0; BETWEEN(head[index], 0x20, 0x30); inter[i++] = head[index++])
		if (head + index >= tail)
			return NULL;
	inter[i] = '\0';

	if (tail <= head + index)
		return NULL;

	/* 終端バイト */
	switch (head[index]) {
	case 0x40: /* ICH 文字挿入 */
		if (line) {
			int n = MAX(atoi(param), 1);
			char32_t str[n];
			int attr[n], fg[n], bg[n];
			INIT(str, L' ');
			INIT(attr, NONE);
			INIT(fg, deffg);
			INIT(bg, defbg);
			InsertLine il = { str, attr, fg, bg };
			insertU32s(line, getIndex(line->str, term->cx), &il, n);
		}
		break;

		/* カーソル移動 */
	case 0x41: moveCursorPos(term, 0, -MAX(atoi(param), 1)); break; /* CUU */
	case 0x42: moveCursorPos(term, 0,  MAX(atoi(param), 1)); break; /* CUD */
	case 0x43: moveCursorPos(term,  MAX(atoi(param), 1), 0); break; /* CUF */
	case 0x44: moveCursorPos(term, -MAX(atoi(param), 1), 0); break; /* CUB */

	case 0x47: /* CHA カーソル文字位置決め */
		setCursorPos(term, atoi(param) - 1, term->cy);
		break;

	case 0x48: /* CUP カーソル位置決め */
		str1 = pp ? mystrsep(&pp, ";:") : "1";
		str2 = pp ? mystrsep(&pp, ";:") : "1";
		setCursorPos(term, atoi(str2) - 1, atoi(str1) - 1);
		break;

	case 0x4a: /* ED ページ内消去 */
		switch (*param) {
		default:
		case '0':
			if (line && 0 < (len = term->sb->cols - term->cx))
				putSPCs(line, term->cx, term->bg, len);
			begin = term->cy + 1;
			end = sb->rows;
			break;
		case '1':
			begin = 0;
			end = term->cy;
			if (line)
				putSPCs(line, 0, term->bg, term->cx + 1);
			break;
		case '2':
			begin = 0;
			end = sb->rows;
			break;
		}
		for (i = begin; i < end; i++)
			if ((line = getLine(term->sb, i)))
				PUT_NUL(line, 0);
		break;

	case 0x4b: /* EL 行内消去 */
		if (!line)
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
		if (line)
			eraseInLine(line, term->cx, MAX(atoi(param), 1));
		break;

	case 0x53: /* SU スクロール上 */
		areaScroll(term, sb->scrs, sb->scre, MAX(atoi(param), 1));
		break;

	case 0x54: /* SD スクロール下 */
		areaScroll(term, sb->scrs, sb->scre, -MAX(atoi(param), 1));
		break;

	case 0x58: /* ECH 文字消去 */
		if (line)
			putSPCs(line, term->cx, term->bg, atoi(param));
		break;

	case 0x63: /* DA 装置識別 */
		if (param[0] == '\0' || param[0] == '0')
			writePty(term, "\e[?6c", 5);
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
		str1 = mystrsep(&pp, ";");
		str2 = mystrsep(&pp, ";");
		a = str1 ? CLIP(atoi(str1), 0, sb->rows) : 1;
		b = str2 ? CLIP(atoi(str2), 0, sb->rows) : sb->rows;
		if (b <= a)
			break;
		sb->scrs = a - 1;
		sb->scre = b - 1;
		break;

	case 0x00: /* NUL このNULを取り除いて読み直す */
		removeCharFromReadbuf(term, head + index);
		return CSI(term, head + 1, tail);

	default: /* 未対応 */
		if (!BETWEEN(head[index], 0x40, 0x7f)) {
			fprintf(stderr, "Invalid CSI: CSI [%s][%s](%#04x)\n",
					param, inter, head[index]);
			return head + index;
		}
		fprintf(stderr, "Not Supported CSI: CSI [%s][%s]%c(%#04x)\n",
				param, inter, head[index], head[index]);
	}

	return head + index + 1;
}

const char *
ctrlSeq(Term *term, const char *head, const char *tail, enum cseq_type type)
{
	const int len = tail - head;
	char payload[len + 1], err[len + 1];
	int i;

	/* 中身を読み取る */
	for (i = 0; i <= len; i++) {
		/* ST(ESC \)またはBELで終了 */
		if ((i < len && strncmp(&head[i], "\e\\", 2) == 0) ||
				(type == CS_OSC && head[i] == 0x07))
			break;
		/* 使えない文字またはSOSが現れて中断 */
		if ((type != CS_SOS && !(BETWEEN(head[i], 0x08, 0x0e) || IS_GC(head[i]))) ||
		    (type == CS_SOS && i < len && strncmp(&head[i], "\eX", 2) == 0)) {
			if (head[i] == 0x00) {
				/* NULが原因なら取り除いて読み直す */
				removeCharFromReadbuf(term, head + i);
				return ctrlSeq(term, head + 1, tail, type);
			} else {
				err[i] = '\0';
				fprintf(stderr, "CtrlSeq \"%s\" was interrupted by '%#x'\n",
						err, head[i]);
				return &head[i];
			}
		}
		/* 末尾に到達して中断 */
		if (len <= i)
			return NULL;
		/* 内容を記録 */
		err[i] = IS_GC(head[i]) ? head[i] : '?';
	}
	strncpy(payload, head, i);
	payload[i] = err[i] = '\0';

	/* 制御列の種類ごとの処理 */
	switch (type) {
	case CS_OSC:     OSC(term, payload, err);                       break;
	case CS_SOS:    CStr(term, payload, err, "SOS");                break;
	case CS_DCS:    CStr(term, payload, err, "DCS");                break;
	case CS_PM:     CStr(term, payload, err, "PM");                 break;
	case CS_APC:    CStr(term, payload, err, "APC");                break;
	case CS_k:      strncpy(term->title, payload, TITLE_MAX - 1);   break;
	default:
	}

	return head + i + (head[i] == 0x07 ? 1 : 2);
}

void
CStr(Term *term, const char *payload, const char *err, const char *type)
{
	fprintf(stderr, "Not Supported %s: %s\n", type, err);
}

void
OSC(Term *term, char *payload, const char *err)
{
	char *spec, *endptr, *buf, res[28];
	char *r, *g, *b;
	int pn, pc, color;

	pn = (buf = mystrsep(&payload, ";")) ? atoi(buf) : -1;
	switch (pn) {
	case 0:  /* タイトル */
		strncpy(term->title, payload, TITLE_MAX - 1);
		return;

	case 4:  /* 色設定 */
		buf = mystrsep(&payload, ";");
	case 10: /* 文字色設定 */
	case 11: /* 背景色設定 */
		switch (pn) {
		case 4:  pc = buf ? atoi(buf) : 0;      break;
		case 10: pc = deffg;                    break;
		case 11: pc = defbg;                    break;
		}
		spec = mystrsep(&payload, ";");
		if (spec == NULL) {
		} else if (strncmp(spec, "#", 1) == 0) {        /* #rrggbb形式 */
			color = strtol(&spec[1], &endptr, 16);
			if (endptr == &spec[7] && spec[7] == '\0') {
				term->palette[pc] &= 0xff000000;
				term->palette[pc] |= color;
				term->palette_cnt++;
			}
		} else if (strncmp(spec, "rgb:", 4) == 0) {     /* rgb:rr/gg/bb形式 */
			r = strtok(&spec[4], "/");
			g = strtok(NULL, "/");
			b = strtok(NULL, "/");
			if (r && g && b) {
				term->palette[pc] &= 0xff000000;
				term->palette[pc] |= (0xff & strtol(r, NULL, 16)) << 16;
				term->palette[pc] |= (0xff & strtol(g, NULL, 16)) << 8;
				term->palette[pc] |= (0xff & strtol(b, NULL, 16));
				term->palette_cnt++;
			}
		} else if (strncmp(spec, "rgbi:", 5) == 0) {    /* rgbi:r/g/b形式 */
			r = strtok(&spec[5], "/");
			g = strtok(NULL, "/");
			b = strtok(NULL, "/");
			if (r && g && b) {
				term->palette[pc] &= 0xff000000;
				term->palette[pc] |= (0xff & (int)(0xff * atof(r))) << 16;
				term->palette[pc] |= (0xff & (int)(0xff * atof(g))) << 8;
				term->palette[pc] |= (0xff & (int)(0xff * atof(b)));
				term->palette_cnt++;
			}
		} else if (strncmp(spec, "reset", 6) == 0) {    /* 元の色に戻す */
			term->palette[pc] = term->def_palette[pc];
			term->palette_cnt++;
		} else if (strncmp(spec, "?", 2) == 0) {        /* 現在の値を返す */
			if (pn == 4)
				snprintf(res, 8, "\e]%d;%d", pn, pc);
			else
				snprintf(res, 8, "\e]%d", pn);
			snprintf(strchr(res, '\0'), 21, ";rgb:%04x/%04x/%04x\a",
					  RED(term->palette[pc]) * 257,
					GREEN(term->palette[pc]) * 257,
					 BLUE(term->palette[pc]) * 257);
			writePty(term, res, strlen(res));
		}
		return;
	}

	/* 未対応 */
	fprintf(stderr, "Not Supported OSC: %s\n", err);
	return;
}

void
linefeed(Term *term)
{
	struct ScrBuf *sb = term->sb;
	Line *line;

	if (term->cy < sb->scre) {
		term->cy++;
	} else if (0 < sb->scrs || sb->scre < sb->rows - 1) {
		areaScroll(term, sb->scrs, sb->scre, 1);
	} else {
		sb->firstline++;
		if (sb->totallines < sb->firstline + sb->rows)
			sb->totallines++;
		if ((line = getLine(term->sb, term->cy)))
			PUT_NUL(line, 0);
	}

	term->sb->am = 0;
}

void
setCursorPos(Term *term, int cx, int cy)
{
	term->cx = CLIP(cx, 0, term->sb->cols - 1);
	term->cy = CLIP(cy, term->sb->scrs, term->sb->scre);
	term->sb->am = 0;
}

void
moveCursorPos(Term *term, int dx, int dy)
{
	setCursorPos(term, term->cx + dx, term->cy + dy);
}

void
areaScroll(Term *term, int first, int last, int num)
{
	struct ScrBuf *sb = term->sb;
	const int area = last - first + 1;
	Line *tmp[area];
	int index;
	int i;

	if (first < 0 || last < first || sb->rows < last)
		return;

	for (i = 0; i < area; i++) {
		index = sb->firstline + first + i;
		tmp[i] = LINE(sb, index);
	}

	for (i = 0; i < area; i++) {
		index = sb->firstline + first + i;

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
	struct ScrBuf *oldsb;
	Line *line;
	int i;

	switch (num) {
	case 1:    /* Application Cursor Keys */
	case 25:   /* Show cursor */
	case 9:    /* Mouse Tracking - X10 */
	case 1000: /* Mouse Tracking - normal */
	case 1002: /* Mouse Tracking - button */
	case 1003: /* Mouse Tracking - any */
	case 1004: /* Focus In/Out */
	case 1005: /* Mouse Tracking - UTF-8 (非対応) */
	case 1006: /* Mouse Tracking - SGR */
	case 1015: /* Mouse Tracking - urxvt (非対応) */
	case 2004: /* Bracketed Paste Mode */
	case 7727: /* Application escape key mode */
		break;

	case 7:    /* Auto-Wrap */
		term->sb->am = 0;
		break;

	case 12:   /* Start blinking cursor */
		term->ctype = (MAX(term->ctype - 1, 0) & ~1) + !flag + 1;
		break;

	case 1047:  /* Alternate Screen Buffer */
	case 1049:  /* Alternate Screen Buffer clear */
		oldsb = term->sb;
		term->sb = flag ? &term->alt : &term->ori;

		if (oldsb == term->sb)
			break;

		if (flag) {
			term->svx = term->cx;
			term->svy = term->cy;
			if (num == 1049)
				for (i = 0; i < term->sb->rows; i++)
					if ((line = getLine(term->sb, i)))
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
	return write(term->master, buf, n);
}

void
setWinSize(Term *term, int row, int col, int xpixel, int ypixel)
{
	struct winsize ws;

	row = CLIP(row, 1, term->sb->maxlines);
	col = MAX(col, 3);
	ws = (struct winsize){ row, col, xpixel, ypixel };

	if (term->sb->rows != row || term->sb->cols != col)
		setScrBufSize(term, row, col);
	ioctl(term->master, TIOCSWINSZ, &ws);
}

void
setScrBufSize(Term *term, int row, int col)
{
	struct ScrBuf *sb = term->sb;
	int newfst = sb->firstline;

	/* 行数が減ってカーソルが画面外に出たとき */
	if (row < sb->rows && row - 1 < term->cy)
		newfst = sb->firstline + (term->cy - row) + 1;
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
setSGR(Term *term, const char *param)
{
	char *buf, *buf2, tokens[strlen(param) + 1], *p = tokens;
	int n;

	strcpy(tokens, param);

	for (buf = mystrsep(&p, ";"); buf; buf = mystrsep(&p, ";")) {
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
		switch (n) {
		case 21: term->attr |= DULINE;                  break;
		case 22: term->attr &= ~(BOLD | FAINT);         break;
		case 23: term->attr &= ~ITALIC;                 break;
		case 24: term->attr &= ~(ULINE | DULINE);       break;
		case 25: term->attr &= ~(BLINK | RAPID);        break;
		case 27: term->attr &= ~NEGA;                   break;
		case 28: term->attr &= ~CONCEAL;                break;
		case 29: term->attr &= ~STRIKE;                 break;
		}

		/* フォント */
		if (BETWEEN(n, 10, 21))
			fprintf(stderr, "font:%d\n", n - 10);

		/* 文字色 */
		if (BETWEEN(n, 30, 38))
			term->fg = n - 30;
		if (BETWEEN(n, 90, 98))
			term->fg = n - 82;
		if (n == 38) {
			buf  = mystrsep(&p, ";");
			buf2 = mystrsep(&p, ";");
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
			buf  = mystrsep(&p, ";");
			buf2 = mystrsep(&p, ";");
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

Line *
getLine(ScrBuf *sb, int row)
{
	const int index = sb->firstline + row;
	const int oldest = MAX(sb->totallines - sb->maxlines, 0);

	if (index < oldest || sb->totallines <= index || sb->rows <= row)
		return NULL;

	return LINE(sb, index);
}

void
getLines(ScrBuf *sb, Line **lines, int len, int scr, const Selection *sel)
{
	Line *line;
	int i, j, s, e, a, b, li, ri;

	/* 指定された範囲をコピー */
	for (i = 0; i < len; i++) {
		if ((line = getLine(sb, i - scr)))
			linecpy(lines[i], line);
		else
			PUT_NUL(lines[i], 0);
	}

	/* selectionの範囲を反転色にする */
	if (sel == NULL || sel->sb != sb)
		return;
	s = MIN(sel->aline, sel->bline) - sb->firstline + scr;
	e = MAX(sel->aline, sel->bline) - sb->firstline + scr;
	for (i = MAX(s, 0); i < MIN(e + 1, len); i++) {
		a = 0;
		b = sb->cols + 2;

		if (sel->rect || (sel->aline == sel->bline)) {
			a = MIN(sel->acol,  sel->bcol);
			b = MAX(sel->acol,  sel->bcol);
		} else if (i == s) {
			a = sel->aline < sel->bline ? sel->acol : sel->bcol;
		} else if (i == e) {
			b = sel->aline < sel->bline ? sel->bcol : sel->acol;
		}

		li = getIndex(lines[i]->str, a);
		ri = getIndex(lines[i]->str, b);
		for (j = li; j < ri && j < u32slen(lines[i]->str); j++)
			lines[i]->attr[j] ^= NEGA;
	}
}

void
setSelection(Selection *sel, ScrBuf *sb, int row, int col, bool start, bool rect)
{
	Line *line;
	int s, e, i;

	/* 範囲をセット */
	if (start) {
		sel->sb = sb;
		sel->acol  = MAX(col,  0);
		sel->aline = MAX(row + sb->firstline, 0);
	} else if (sel->sb != sb) {
		return;
	}
	sel->bcol  = MAX(col,  0);
	sel->bline = MAX(row + sb->firstline, 0);
	sel->rect = rect;

	/* 行のバージョンを記録 */
	s = MIN(sel->aline, sel->bline);
	e = MAX(sel->aline, sel->bline);
	sel->vers = xrealloc(sel->vers, (e - s + 1) * sizeof(int));
	for (i = 0; i <= e - s; i++)
		if ((line = getLine(sb, s - sb->firstline + i)))
			sel->vers[i] = line->ver;
}

bool
checkSelection(Selection *sel)
{
	const int s = MIN(sel->aline, sel->bline);
	const int e = MAX(sel->aline, sel->bline);
	Line *line;
	int i, first = sel->sb->firstline;

	for (i = 0; i <= e - s; i++) {
		line = getLine(sel->sb, s - first + i);
		if (line != NULL && line->ver != sel->vers[i])
			return true;
	}

	return false;
}

void
copySelection(Selection *sel, char **dst, bool deltrail)
{
	int len = 256;
	char32_t *cp, *copy = xmalloc(len * sizeof(copy[0]));
	const int firstline = MIN(sel->aline, sel->bline);
	const int lastline  = MAX(sel->aline, sel->bline);
	const int left      = MIN(sel->acol,  sel->bcol);
	const int right     = MAX(sel->acol,  sel->bcol);
	Line *line;
	int i, j, l, r;

	copy[0] = L'\0';

	/* 選択範囲の文字列(UTF32)を読み出してコピー */
	for(i = firstline; i <= lastline; i++) {
		if (!(line = getLine(sel->sb, i - sel->sb->firstline)))
			continue;

		l = MIN(getIndex(line->str,  left), u32slen(line->str));
		r = MIN(getIndex(line->str, right), u32slen(line->str));
		if (!sel->rect) {
			l = (i == firstline) ? l : 0;
			r = (i ==  lastline) ? r : u32slen(line->str) + 1;
		}

		while (len < u32slen(copy) + r - l + 2) {
			len += 256;
			copy = xrealloc(copy, len * sizeof(copy[0]));
		}
		cp = copy + u32slen(copy);
		wcsncpy((wchar_t *)cp, (wchar_t *)line->str + l, r - l);
		cp[r - l] = L'\0';

		if (deltrail) {
			for (j = u32slen(cp); 0 < j; j--)
				if (cp[j - 1] != L' ')
					break;
			cp[j] = L'\0';
		}

		if (i < lastline)
			wcscat((wchar_t *)copy, L"\n");
	}

	/* UTF8に変換して保存 */
	*dst = xrealloc(*dst, len * 4);
	wcstombs(*dst, (wchar_t *)copy, len * 4);

	free(copy);
}
