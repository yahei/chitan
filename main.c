#include <sys/select.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include "term.h"
#include "util.h"

enum { NORMAL_FONT, BOLD_FONT, ITALIC_FONT, BOLD_ITALIC_FONT, FONT_NUM };
static const int weights[] = { XFT_WEIGHT_MEDIUM, XFT_WEIGHT_BOLD, XFT_WEIGHT_MEDIUM, XFT_WEIGHT_BOLD };
static const int slants[] = { XFT_SLANT_ROMAN, XFT_SLANT_ROMAN, XFT_SLANT_ITALIC, XFT_SLANT_ITALIC };

typedef struct XFont {
	Display *disp;
	XftFont *fonts[FONT_NUM];
	int cw, ch;
} XFont;

typedef struct Win {
	Term *term;
	Window window;
	GC gc;
	XftDraw *draw;
	XClassHint *hint;
	int width, height;
	int xpad, ypad;
	int col, row;
	struct {
		XIC xic;
		XICCallback icdestroy;
		XIMCallback pestart;
		XIMCallback pedone;
		XIMCallback pedraw;
		XIMCallback pecaret;
		XPoint spot;
		XVaNestedList spotlist;
		XVaNestedList peattrs;
		Line *peline;
		int caret;
	} ime;
} Win;

static Display *disp;
static Visual *visual;
static Colormap cmap;
static XFont *xfont;
static XIM xim;
static Win *win;

static void init(void);
static void run(void);
static void fin(void);

/* Font */
static XFont *openFont(Display *, const char *, float);
static void closeFont(XFont *);

/* Win */
static Win *openWindow(void);
static void closeWindow(Win *);
static void procXEvent(Win *);
static void procKeyPress(Win *, XEvent, int);
static void redraw(Win *);
static void drawLine(Win *, Line *, int, int);
static void drawString(Win *, int, int, const char32_t *, int, int, int, int);
static void drawCursor(Win *, int, int, int, Line *, int);

/* IME */
static void ximOpen(Display *, XPointer, XPointer);
static void imDestroy(XIM, XPointer, XPointer);
static void xicOpen(Win *);
static int icDestroy(XIC, Win *, XPointer);
static void preeditStart(XIM, Win *, XPointer);
static void preeditDone(XIM, Win *, XPointer);
static void preeditDraw(XIM, Win *, XIMPreeditDrawCallbackStruct *);
static void preeditCaret(XIM, Win *, XIMPreeditCaretCallbackStruct *);

/* Selection */
enum { PRIMARY, CLIPBOARD, UTF8_STRING, MY_SELECTION, ATOM_NUM };
Atom atoms[ATOM_NUM];

int
main(int argc, char *args[])
{
	init();
	run();
	fin();
	return 0;
}

void
init(void)
{
	char **names;
	int i;

	/* localeを設定 */
	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers("");

	/* Xサーバーに接続 */
	disp= XOpenDisplay(NULL);
	if (disp== NULL)
		fatal("XOpenDisplay failed.\n");
	visual = DefaultVisual(disp, 0);
	cmap = DefaultColormap(disp, 0);

	/* XIM */
	XRegisterIMInstantiateCallback(disp, NULL, NULL, NULL,
			ximOpen, NULL);

	/* 文字描画の準備 */
	FcInit();
	xfont = openFont(disp, "monospace", 11.5);
	if (xfont == NULL)
		fatal("XftFontOpen failed.\n");

	/* Selection */
	names = (char *[]){ "PRIMARY", "CLIPBOARD", "UTF8_STRING", "_MY_SELECTION_" };
	for (i = 0; i < ATOM_NUM; i++)
		atoms[i] = XInternAtom(disp, names[i], True);
	
	/* ウィンドウの作成 */
	win = openWindow();
}

void
run(void)
{
	fd_set rfds;
	struct timespec timeout, *ptimeout = NULL;
	int tfd = win->term->master;
	int xfd = XConnectionNumber(disp);

	while (1) {
		/* ファイルディスクリプタの監視 */
		FD_ZERO(&rfds);
		FD_SET(tfd, &rfds);
		FD_SET(xfd, &rfds);
		if (pselect(MAX(tfd, xfd) + 1, &rfds, NULL, NULL, ptimeout, NULL) < 0) {
			if (errno == EINTR)
				fprintf(stderr, "signal.\n");
			else
				errExit("pselect failed.\n");
		}

		/* 端末のread */
		if (FD_ISSET(tfd, &rfds))
			if (readPty(win->term) < 0)
				return;

		/* ウィンドウのイベント処理 */
		if (FD_ISSET(xfd, &rfds) || XPending(disp))
			procXEvent(win);

		/* 再描画 */
		if (!FD_ISSET(tfd, &rfds) && !FD_ISSET(xfd, &rfds))
			redraw(win);

		timeout = (struct timespec) { 0, 0 };
		ptimeout = FD_ISSET(tfd, &rfds) ? &timeout : NULL;
	}
}

void
fin(void)
{
	closeWindow(win);
	closeFont(xfont);
	FcFini();
	if (xim)
		XCloseIM(xim);
	XCloseDisplay(disp);
}

XFont *
openFont(Display *disp, const char *name, float size)
{
	XFont *xfont = xmalloc(sizeof(XFont));
	XGlyphInfo ginfo;
	int i;

	xfont->disp = disp;

	/* フォントを読み込む */
	for (i = 0; i < FONT_NUM; i++)
		xfont->fonts[i] = XftFontOpen(
				disp, 0,
				XFT_FAMILY, XftTypeString, name,
				XFT_SIZE, XftTypeDouble, size,
				XFT_WEIGHT, XftTypeInteger, weights[i],
				XFT_SLANT, XftTypeInteger, slants[i],
				NULL);

	for (i = 0; i < FONT_NUM; i++) {
		if (xfont->fonts[i] == NULL) {
			closeFont(xfont);
			return NULL;
		}
	}

	/* テキストの高さや横幅を取得 */
	xfont->ch = xfont->fonts[NORMAL_FONT]->height - 2.0;
	XftTextExtents32(disp, xfont->fonts[NORMAL_FONT], (char32_t *)L"x", 1, &ginfo);
	xfont->cw = ginfo.width;

	return xfont;
}

void
closeFont(XFont *xfont) {
	int i;

	for (i = 0; i < FONT_NUM; i++)
		if (xfont->fonts[i])
			XftFontClose(xfont->disp, xfont->fonts[i]);
	free(xfont);
}

Win *
openWindow(void)
{
	Win *win = xmalloc(sizeof(Win));

	/* 端末をオープン */
	win->term = openTerm();
	if (!win->term)
		errExit("newTerm failed.\n");

	/* ウィンドウ作成 */
	win->width = 800;
	win->height = 600;
	win->xpad = 10;
	win->ypad = 10;
	win->window = XCreateSimpleWindow(
			disp,
			DefaultRootWindow(disp),
			10, 10, win->width, win->height, 1,
			win->term->palette[deffg],
			win->term->palette[defbg]);

	/* プロパティ */
	win->hint = XAllocClassHint();
	win->hint->res_name = "minty";
	win->hint->res_class = "minty";
	XSetClassHint(disp, win->window, win->hint);

	/* イベントマスク */
	XSelectInput(disp, win->window,
			ExposureMask | StructureNotifyMask |
			KeyPressMask | KeyReleaseMask |
			ButtonPressMask | ButtonReleaseMask |
			ButtonMotionMask | PointerMotionMask);

	/* 描画の準備 */
	win->gc = XCreateGC(disp, win->window, 0, NULL);
	win->draw = XftDrawCreate(disp, win->window, visual, cmap);
	XSetForeground(disp, win->gc, win->term->palette[deffg]);
	XSetBackground(disp, win->gc, win->term->palette[defbg]);

	/* IME */
	xicOpen(win);
	win->ime.spotlist = XVaCreateNestedList(0,
			XNSpotLocation, &win->ime.spot,
			NULL);
	win->ime.peline = allocLine();

	/* ウィンドウを表示 */
	XMapWindow(disp, win->window);
	XFlush(disp);

	return win;
}

void
closeWindow(Win *win)
{
	freeLine(win->ime.peline);
	XFree(win->ime.spotlist);
	XFree(win->ime.peattrs);
	if (win->ime.xic)
		XDestroyIC(win->ime.xic);
	XftDrawDestroy(win->draw);
	XFreeGC(disp, win->gc);
	XFree(win->hint);
	XDestroyWindow(disp, win->window);
	closeTerm(win->term);
	free(win);
}

void
procXEvent(Win *win)
{
	XEvent event;
	XConfigureEvent *e;
	int state, mb;
	Atom prop, type;
	int format;
	unsigned long ntimes, after;
	unsigned char *props;

	while (0 < XPending(disp)) {
		XNextEvent(disp, &event);

		/* IMEのイベントをフィルタリング */
		if (!xim)
			ximOpen(disp, NULL, NULL);
		if (xim && XFilterEvent(&event, None) == True)
			continue;

		switch (event.type) {
		case KeyPress:
			/* 端末に文字を送る */
			procKeyPress(win, event, 64);
			break;

		case ButtonPress:
		case ButtonRelease:
		case MotionNotify:
			/* マウス操作 */
			state = event.xbutton.state;
			if (event.type == MotionNotify) {
				mb = (state & Button1Mask ?  0 :
				      state & Button2Mask ?  1 :
				      state & Button3Mask ?  2 : 3) + MOVE;
			} else {
				mb = event.xbutton.button;
				mb = BETWEEN(mb, 1, 4)  ? (mb - 1) :
				     BETWEEN(mb, 4, 8)  ? (mb - 4) + WHEEL :
				     BETWEEN(mb, 8, 12) ? (mb - 8) + OTHER : 3;
			}
			mb += state & ShiftMask   ? SHIFT : 0;
			mb += state & Mod1Mask    ? ALT   : 0;
			mb += state & ControlMask ? CTRL  : 0;
			reportMouse(win->term, mb, event.type == ButtonRelease,
					(event.xbutton.x - win->xpad) / xfont->cw + 1,
					(event.xbutton.y - win->ypad) / xfont->ch + 1);
			break;

		case Expose:
			/* 画面を再描画する */
			redraw(win);
			break;

		case ConfigureNotify:
			/* ウィンドウサイズ変更 */
			e = (XConfigureEvent *)&event;
			win->width = e->width;
			win->height = e->height;
			win->col = (e->width - win->xpad * 2) / xfont->cw;
			win->row = (e->height - win->ypad * 2) / xfont->ch;
			setWinSize(win->term, win->row, win->col,
					win->width, win->height);
			break;

		case SelectionNotify:
			/* 貼り付け */
			if ((prop = event.xselection.property) != None) {
				XGetWindowProperty(disp, win->window, prop, 0, 256,
						False, atoms[UTF8_STRING], &type,
						&format, &ntimes, &after, &props);
				if (1 < win->term->dec[2004])
					writePty(win->term, "\e[200~", 6);
				writePty(win->term, (char *)props, ntimes);
				if (1 < win->term->dec[2004])
					writePty(win->term, "\e[201~", 6);
				XFree(props);
			}
		}
	}
}

void
procKeyPress(Win *win, XEvent event, int bufsize)
{
	char buf[bufsize], str[16];
	int len;
	KeySym keysym;
	Status status = XLookupChars;

	/* 入力文字列を取得 */
	len = win->ime.xic ?
		Xutf8LookupString(win->ime.xic, &event.xkey, buf, bufsize, &keysym, &status) :
		XLookupString(&event.xkey, buf, bufsize, &keysym, NULL);
	if (status == XBufferOverflow)
		return procKeyPress(win, event, len);

	/* C-S-vで貼り付け */
	if (keysym == XK_V && event.xkey.state & ControlMask) {
		XConvertSelection(disp, atoms[CLIPBOARD], atoms[UTF8_STRING], atoms[MY_SELECTION],
				win->window, event.xkey.time);
		return;
	}

	if (strlen(buf)) {
		/* 文字列を送る */
		if (event.xkey.state & Mod1Mask)
			writePty(win->term, "\e", 1);
		writePty(win->term, buf, len);
	} else {
		/* カーソルキーを送る */
		switch (keysym) {
		case XK_Up:
		case XK_Down:
		case XK_Right:
		case XK_Left:
			snprintf(str, sizeof(str), "\e%c%c",
					1 < win->term->dec[1] ? 'O' : '[',
					"DACB"[keysym - XK_Left]);
			break;
		default:
			return;
		}
		writePty(win->term, str, strlen(str));
	}
}

void
redraw(Win *win)
{
	XWindowAttributes wattr;
	Line *line;
	int x, y, pepos, pewidth, pecaretpos;
	int i;

	/* 画面をクリア */
	XGetWindowAttributes(disp, win->window, &wattr);
	XClearArea(disp, win->window, 0, 0, wattr.width, wattr.height, False);

	/* 端末の内容をウィンドウに表示 */
	for (i = 0; (line = getLine(win->term, i)); i++)
		drawLine(win, line, win->xpad,
				win->ypad + i * xfont->ch + xfont->fonts[NORMAL_FONT]->ascent);

	/* カーソルの位置を取得 */
	x = win->xpad + win->term->cx * xfont->cw;
	y = win->ypad + win->term->cy * xfont->ch + xfont->fonts[NORMAL_FONT]->ascent;

	/* カーソルかPreeditを表示 */
	XSetForeground(disp, win->gc, win->term->palette[deffg]);
	if (u32slen(win->ime.peline->str)) {
		/* Preeditの幅とキャレットのPreedit内での位置を取得 */
		pewidth = u32swidth(win->ime.peline->str, u32slen(win->ime.peline->str));
		pecaretpos = u32swidth(win->ime.peline->str, win->ime.caret);

		/* Preeditの画面上での描画位置を決める */
		pepos = win->term->cx - pecaretpos;
		pepos = MIN(pepos, 0);
		pepos = MAX(pepos, win->col - pewidth);
		pepos = MIN(pepos, win->term->cx);

		/* Preeditとカーソルの描画 */
		drawLine(win, win->ime.peline, win->xpad + xfont->cw * pepos, y);
		drawCursor(win, win->xpad + xfont->cw * (pepos + pecaretpos),
				y, 6, win->ime.peline, win->ime.caret);
	} else if (1 <= win->term->dec[25]) {
		/* カーソルの描画 */
		line = getLine(win->term, win->term->cy);
		drawCursor(win, x, y, win->term->ctype,
				line, getCharCnt(line, win->term->cx).index);
	}

	/* スポット位置 */
	if (win->ime.xic) {
		win->ime.spot.x = x;
		win->ime.spot.y = y;
		XSetICValues(win->ime.xic, XNPreeditAttributes, win->ime.spotlist, NULL);
	}

	XFlush(disp);
}

void
drawLine(Win *win, Line *line, int xoff, int yoff)
{
	int fg, bg;
	int x, y;
	int i;

	/* 1文字ずつ座標を指定して書く */
	for (i = 0; line->str[i] != L'\0'; i++) {
		/* 描画する文字列 */
		fg = line->fg[i];
		bg = line->bg[i];
		x = xoff + xfont->cw * u32swidth(line->str, i);
		y = yoff;

		/* 前処理 */
		if (line->attr[i] & NEGA) { /* 反転 */
			fg = fg ^ bg;
			bg = fg ^ bg;
			fg = fg ^ bg;
		}
		if (line->attr[i] & BOLD) { /* 太字 */
			fg = fg < 8 ? fg + 8 : fg;
		}

		/* 描画 */
		drawString(win, x, y, &line->str[i], 1, line->attr[i], fg, bg);
	}
}

void
drawString(Win *win, int x, int y, const char32_t *str, int len, int attr, int fg, int bg)
{
	XftFont *font;
	int width;
	XftColor xc;
	Color c;

	/* 使うフォント */
	if ((attr & BOLD) && (attr & ITALIC))
		font = xfont->fonts[BOLD_ITALIC_FONT];
	else if (attr & BOLD)
		font = xfont->fonts[BOLD_FONT];
	else if (attr & ITALIC)
		font = xfont->fonts[ITALIC_FONT];
	else
		font = xfont->fonts[NORMAL_FONT];

	/* 文字列の幅 */
	width = xfont->cw * u32swidth(str, len);

	/* 背景 */
	XSetForeground(disp, win->gc, win->term->palette[bg]);
	XFillRectangle(disp, win->window, win->gc,
			x, y - font->ascent + 1, width, xfont->ch);

	/* 文字 */
	c = win->term->palette[fg];
	xc.color.red   =   RED(c) << 8;
	xc.color.green = GREEN(c) << 8;
	xc.color.blue  =  BLUE(c) << 8;
	xc.color.alpha = 0xffff;
	XftDrawString32(win->draw, &xc, font, x, y, str, len);

	/* 後処理 */
	if (attr & ULINE) { /* 下線 */
		XSetForeground(disp, win->gc, win->term->palette[fg]);
		XDrawLine(disp, win->window, win->gc, x, y + 1, x + width, y + 1);
	}
}

void
drawCursor(Win *win, int x, int y, int type, Line *line, int index)
{
	int width = xfont->cw * u32swidth(&line->str[index], 1);

	switch (type) {
	default: /* ブロック */
	case 0:
	case 1:
	case 2:
		if (index < u32slen(line->str))
			drawString(win, x, y, &line->str[index], 1, line->attr[index], defbg, deffg);
		else
			drawString(win, x, y, (char32_t *)L" ", 1, 0, defbg, deffg);
		break;
	case 3: /* 下線 */
	case 4:
		XFillRectangle(disp, win->window, win->gc, x, y + 1, width, xfont->ch * 0.1);
		break;
	case 5: /* 縦線 */
	case 6:
		y = y - xfont->fonts[NORMAL_FONT]->ascent + 1;
		XFillRectangle(disp, win->window, win->gc, x, y, xfont->ch * 0.1, xfont->ch);
		break;
	}
}

void
ximOpen(Display *disp, XPointer, XPointer)
{
	static XIMCallback destroyCB = { NULL, imDestroy };

	xim = XOpenIM(disp, NULL, NULL, NULL);
	if (!xim)
		return;

	if (XSetIMValues(xim, XNDestroyCallback, &destroyCB, NULL))
		fprintf(stderr, "Could not set XNDestroyCallback.\n");

	if (win)
		xicOpen(win);
}

void
imDestroy(XIM _xim, XPointer client, XPointer call)
{
	xim = NULL;
}

void
xicOpen(Win *win)
{
	if (!xim) {
		win->ime.peattrs = NULL;
		return;
	}

	XIMStyles *styles;
	XIMStyle candidates[] = {
		XIMPreeditCallbacks | XIMStatusNothing,
		XIMPreeditNothing | XIMStatusNothing
	};
	int i, j;

	/* コールバック */
	win->ime.icdestroy = (XICCallback){ (XPointer)win, (XICProc)icDestroy };
	win->ime.pestart   = (XIMCallback){ (XPointer)win, (XIMProc)preeditStart };
	win->ime.pedone    = (XIMCallback){ (XPointer)win, (XIMProc)preeditDone };
	win->ime.pedraw    = (XIMCallback){ (XPointer)win, (XIMProc)preeditDraw };
	win->ime.pecaret   = (XIMCallback){ (XPointer)win, (XIMProc)preeditCaret };

	/* 利用可能なスタイルを調べて選ぶ */
	if (XGetIMValues(xim, XNQueryInputStyle, &styles, NULL)) {
		fprintf(stderr, "Could not get XNQueryInputStyle.\n");
		return;
	}
	for (i = 0; i < sizeof(candidates) / sizeof(XIMStyle); i++)
		for (j = 0; j < styles->count_styles; j++)
			if (candidates[i] == styles->supported_styles[j])
				goto match;
	fprintf(stderr, "None of the candidates styles matched.\n");
	XFree(styles);
	return;
match:
	XFree(styles);

	/* コンテキスト作成 */
	win->ime.xic = XCreateIC(xim,
			XNInputStyle, candidates[i],
			XNClientWindow, win->window,
			XNFocusWindow, win->window,
			XNDestroyCallback, &win->ime.icdestroy,
			NULL);
	if (win->ime.xic && candidates[i] & XIMPreeditCallbacks) {
		win->ime.peattrs = XVaCreateNestedList(0,
				XNPreeditStartCallback, &win->ime.pestart,
				XNPreeditDoneCallback,  &win->ime.pedone,
				XNPreeditDrawCallback,  &win->ime.pedraw,
				XNPreeditCaretCallback, &win->ime.pecaret,
				NULL);
		XSetICValues(win->ime.xic, XNPreeditAttributes,
				win->ime.peattrs, NULL);
	} else {
		win->ime.peattrs = NULL;
	}
}

int
icDestroy(XIC xic, Win *win, XPointer call)
{
	win->ime.xic = NULL;
	XFree(win->ime.peattrs);
	win->ime.peattrs = NULL;
	PUT_NUL(win->ime.peline, 0);
	redraw(win);
	return 1;
}

void
preeditStart(XIM xim, Win *win, XPointer call)
{
	PUT_NUL(win->ime.peline, 0);
}

void
preeditDone(XIM xim, Win *win, XPointer call)
{
	PUT_NUL(win->ime.peline, 0);
	redraw(win);
}

void
preeditDraw(XIM xim, Win *win, XIMPreeditDrawCallbackStruct *call)
{
	char32_t *str;
	int *attr, *fg, *bg, defattr;
	int len, oldlen = u32slen(win->ime.peline->str);
	XIMFeedback fb;
	InsertLine newline;
	int i;

	/* カーソル位置 */
	win->ime.caret = call->caret;

	/* 削除の処理 */
	deleteChars(win->ime.peline, call->chg_first, call->chg_length);

	if (call->text == NULL)
		return;

	/* 挿入の処理 */
	len = call->text->length;
	str  = xmalloc(len * sizeof(char32_t));
	attr = xmalloc(len * sizeof(int));
	fg   = xmalloc(len * sizeof(int));
	bg   = xmalloc(len * sizeof(int));

	/* 文字列 */
	u8sToU32s(str, call->text->string.multi_byte, len);

	/* 属性 */
	defattr = ULINE;
	if (0 < oldlen)
		defattr = win->ime.peline->attr[MIN(call->chg_first, oldlen - 1)];
	for (i = 0; i < len; i++) {
		attr[i] = defattr;
		fg[i]   = deffg;
		bg[i]   = defbg;
	}
	if (call->text->feedback) {
		for (i = 0; i < len; i++) {
			fb = call->text->feedback[i];
			attr[i] = NONE;
			attr[i] |= fb & XIMReverse   ? NEGA    : NONE;
			attr[i] |= fb & XIMUnderline ? ULINE   : NONE;
			attr[i] |= fb & XIMHighlight ? BOLD    : NONE;
		}
	}

	/* 挿入を実行 */
	newline = (InsertLine){ str, attr, fg, bg };
	insertU32s(win->ime.peline, call->chg_first, &newline, len);

	/* 終了 */
	free(str);
	free(attr);
	free(fg);
	free(bg);

	redraw(win);
}

void
preeditCaret(XIM xim, Win *win, XIMPreeditCaretCallbackStruct *call)
{
	win->ime.caret = call->position;
	redraw(win);
}
