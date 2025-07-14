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

typedef struct Win {
	Term *term;
	Window window;
	GC gc;
	XftDraw *draw;
	XClassHint *hint;
	int width, height;
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

Display *disp;
Visual *visual;
Colormap cmap;
XftFont *font;
int charx, chary;
XIM xim;
Win *win;

static void init(void);
static void run(void);
static void fin(void);

/* Win */
static Win *openWindow(void);
static void closeWindow(Win *);
static void procXEvent(Win *);
static void procKeyPress(Win *, XEvent, int);
static void redraw(Win *);
static void drawLine(Win *, Line *, int, int);

/* IME */
static void ximOpen(Display *, XPointer, XPointer);
static void imDestroy(XIM, XPointer, XPointer);
static void xicOpen(Win *);
static int icDestroy(XIC, Win *, XPointer);
static void preeditStart(XIM, Win *, XPointer);
static void preeditDone(XIM, Win *, XPointer);
static void preeditDraw(XIM, Win *, XIMPreeditDrawCallbackStruct *);
static void preeditCaret(XIM, Win *, XIMPreeditCaretCallbackStruct *);

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
	XGlyphInfo ginfo;

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
	font = XftFontOpen(
			disp, 0,
			XFT_FAMILY, XftTypeString, "monospace",
			XFT_SIZE, XftTypeDouble, 11.5,
			NULL);
	if (font == NULL)
		fatal("XftFontOpen failed.\n");
	
	/* テキストの高さや横幅を取得 */
	XftTextExtents32(disp, font, (char32_t *)L"plmM", 4, &ginfo);
	chary = ginfo.height * 1.25;
	charx = ginfo.width / 4;

	/* ウィンドウの作成 */
	win = openWindow();
}

void
run(void)
{
	fd_set rfds;
	struct timespec timeout, *ptimeout;
	int tfd = win->term->master;
	int xfd = XConnectionNumber(disp);

	while (1) {
		/* ファイルディスクリプタの監視 */
		FD_ZERO(&rfds);
		FD_SET(tfd, &rfds);
		FD_SET(xfd, &rfds);
		timeout.tv_sec = 0;
		timeout.tv_nsec = 0;
		ptimeout = (0 < timeout.tv_sec || 0 < timeout.tv_nsec) ?
			&timeout : NULL;

		if (pselect(MAX(tfd, xfd) + 1, &rfds, NULL, NULL, ptimeout, NULL) < 0) {
			if (errno == EINTR) {
				/* シグナル受信 */
				fprintf(stderr, "signal.\n");
			} else {
				/* その他のエラー */
				errExit("pselect failed.\n");
			}
		}

		/* 端末のread */
		if (FD_ISSET(tfd, &rfds)) {
			if (readPty(win->term) < 0) {
				/* 終了 */
				printf("exit.\n");
				return;
			}
			redraw(win);
		}

		/* ウィンドウのイベント処理 */
		if (FD_ISSET(xfd, &rfds)) {
			procXEvent(win);
		}
	}
}

void
fin(void)
{
	closeWindow(win);
	XftFontClose(disp, font);
	FcFini();
	if (xim)
		XCloseIM(xim);
	XCloseDisplay(disp);
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
			ExposureMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask);

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

		case Expose:
			/* 画面を再描画する */
			redraw(win);
			break;

		case ConfigureNotify:
			/* ウィンドウサイズ変更 */
			e = (XConfigureEvent *)&event;
			win->width = e->width;
			win->height = e->height;
			setWinSize(win->term, (e->height - 10) / chary,
					(e->width - 20) / charx,
					e->width, e->height);
			break;
		}
	}
}

void
procKeyPress(Win *win, XEvent event, int bufsize)
{
	char buf[bufsize];
	int len;
	Status status;

	if (win->ime.xic) {
		len = Xutf8LookupString(win->ime.xic, &event.xkey,
				buf, bufsize, NULL, &status);
		if (status == XBufferOverflow)
			return procKeyPress(win, event, len);
	} else {
		len = XLookupString(&event.xkey, buf, bufsize, NULL, NULL);
	}

	/* Alt */
	if (event.xkey.state & Mod1Mask)
		writePty(win->term, "\e", 1);

	writePty(win->term, buf, len);
}

void
redraw(Win *win)
{
	XGlyphInfo ginfo;
	XWindowAttributes wattr;
	Line *line;
	int x, y, pepos, pewidth;
	int index;
	int i;

	/* 画面をクリア */
	XGetWindowAttributes(disp, win->window, &wattr);
	XClearArea(disp, win->window, 0, 0, wattr.width, wattr.height, False);

	/* 端末の内容をウィンドウに表示 */
	for (i = 0; (line = getLine(win->term, i)); i++)
		drawLine(win, line, 10, (i + 1) * chary);

	/* カーソルの位置を取得 */
	line = getLine(win->term, win->term->cy);
	index = getCharCnt(line, win->term->cx).index;
	XftTextExtents32(disp, font, line->str, index, &ginfo);
	x = ginfo.xOff + 10;
	y = win->term->cy * chary;

	/* カーソルかPreeditを表示 */
	XSetForeground(disp, win->gc, win->term->palette[deffg]);
	if (u32slen(win->ime.peline->str)) {
		/* Preeditの幅とキャレットのPreedit内での座標を取得 */
		XftTextExtents32(disp, font, win->ime.peline->str, u32slen(win->ime.peline->str), &ginfo);
		pewidth = ginfo.xOff;
		XftTextExtents32(disp, font, win->ime.peline->str, win->ime.caret, &ginfo);

		/* Preeditの画面上での描画位置を決める */
		pepos = x - ginfo.xOff;
		pepos = MIN(pepos, 10);
		pepos = MAX(pepos, win->width - pewidth - 10);
		pepos = MIN(pepos, x);

		/* Preeditとカーソルの描画 */
		drawLine(win, win->ime.peline, pepos, y + chary);
		XDrawRectangle(disp, win->window, win->gc, pepos + ginfo.xOff, y + chary/4, charx, chary);
	} else {
		/* カーソルの描画 */
		XDrawRectangle(disp, win->window, win->gc, x, y + chary/4, charx, chary);
	}

	/* スポット位置 */
	if (win->ime.xic) {
		win->ime.spot.x = x;
		win->ime.spot.y = y + chary;
		XSetICValues(win->ime.xic, XNPreeditAttributes, win->ime.spotlist, NULL);
	}

	XFlush(disp);
}

void
drawLine(Win *win, Line *line, int xoff, int yoff)
{
	XGlyphInfo ginfo;
	int current, next;
	int fg, bg;
	XftColor xc;
	Color c;
	int x, y, width;
	const char32_t *str;
	int len;

	/* 同じSGRの文字列ごとにまとめて書く */
	for (current = 0; line->str[current] != L'\0'; current = next) {
		/* 描画する文字列 */
		next = findNextSGR(line, current);
		fg = line->fg[current];
		bg = line->bg[current];
		str = line->str + current;
		len = next - current;
		XftTextExtents32(disp, font, line->str, current, &ginfo);
		x = ginfo.xOff + xoff;
		y = yoff;
		XftTextExtents32(disp, font, str, len, &ginfo);
		width = ginfo.xOff;

		/* 前処理 */
		if (line->attr[current] & BOLD) { /* 太字 */
			fg = fg < 8 ? fg + 8 : fg;
			bg = bg < 8 ? bg + 8 : bg;
		}
		if (line->attr[current] & NEGA) { /* 反転 */
			fg = fg ^ bg;
			bg = fg ^ bg;
			fg = fg ^ bg;
		}

		/* 背景 */
		XSetForeground(disp, win->gc, win->term->palette[bg]);
		XFillRectangle(disp, win->window, win->gc,
				x, yoff - chary * 0.8, width, chary);

		/* 文字 */
		c = win->term->palette[fg];
		xc.color.red   =   RED(c) << 8;
		xc.color.green = GREEN(c) << 8;
		xc.color.blue  =  BLUE(c) << 8;
		xc.color.alpha = 0xffff;
		XftDrawString32(win->draw, &xc, font, x, y, str, len);

		/* 後処理 */
		if (line->attr[current] & ULINE) { /* 下線 */
			XSetForeground(disp, win->gc, win->term->palette[fg]);
			XDrawLine(disp, win->window, win->gc, x, yoff, x + width, yoff);
		}
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
