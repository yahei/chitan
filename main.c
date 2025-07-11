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
	Window window;
	GC gc;
	XftDraw *draw;
	XIC xic;
	XPoint spot;
	XVaNestedList spotlist;
} Win;

Display *disp;
Visual *visual;
Colormap cmap;
XftFont *font;
Win *win;
XClassHint *hint;
XIM xim;
Term *term;
int charx, chary;

static void init(void);
static void imInstantiateCallback(Display *, XPointer, XPointer);
static void imDestroyCallback(XIM, XPointer, XPointer);
static XIC xicOpen(const Win *);
static void run(void);
static void fin(void);
static void procXEvent(void);
static void redraw(void);
static void drawLine(Line *line, int, int);
static Win *openWindow(void);
static void closeWindow(Win *);

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
			imInstantiateCallback, NULL);

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

	/* 端末をオープン */
	term = openTerm();
	if (!term)
		errExit("newTerm failed.\n");
	
	/* ウィンドウの作成 */
	hint = XAllocClassHint();
	win = openWindow();
}

void
imInstantiateCallback(Display *disp, XPointer, XPointer)
{
	static XIMCallback destroyCB = {NULL, imDestroyCallback};

	xim = XOpenIM(disp, NULL, NULL, NULL);
	if (!xim)
		return;

	XSetIMValues(xim, XNDestroyCallback, &destroyCB, NULL);

	if (win)
		win->xic = xicOpen(win);
}

void
imDestroyCallback(XIM, XPointer, XPointer)
{
	xim = NULL;
	win->xic = NULL;
}

XIC
xicOpen(const Win *win)
{
	XIC xic = XCreateIC(xim,
			XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
			XNClientWindow, win->window,
			XNFocusWindow, win->window,
			NULL);
	return xic;
}

void
run(void)
{
	fd_set rfds;
	struct timespec timeout, *ptimeout;
	int tfd = term->master;
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
			if (readPty(term) < 0) {
				/* 終了 */
				printf("exit.\n");
				return;
			}
			redraw();
		}

		/* ウィンドウのイベント処理 */
		if (FD_ISSET(xfd, &rfds)) {
			procXEvent();
		}
	}
}

void
fin(void)
{
	if (win->xic)
		XDestroyIC(win->xic);
	win->xic = NULL;
	XFree(hint);
	hint = NULL;
	closeWindow(win);
	win = NULL;
	closeTerm(term);
	term = NULL;
	XftFontClose(disp, font);
	FcFini();
	if (xim)
		XCloseIM(xim);
	xim = NULL;
	XCloseDisplay(disp);
}

void
procXEvent(void)
{
	XEvent event;
	XConfigureEvent *e;
	char buf[256];
	int len;

	while (0 < XPending(disp)) {
		XNextEvent(disp, &event);

		/* IMEのイベントをフィルタリング */
		if (!xim)
			imInstantiateCallback(disp, NULL, NULL);
		if (xim && XFilterEvent(&event, None) == True)
			continue;

		switch (event.type) {
		case KeyPress:
			/* 端末に文字を送る */
			if (win->xic)
				len = Xutf8LookupString(win->xic, &event.xkey, buf,
						sizeof(buf) - 1, NULL, NULL);
			else
				len = XLookupString(&event.xkey, buf,
						sizeof(buf) - 1, NULL, NULL);

			if (event.xkey.state & Mod1Mask)
				writePty(term, "\e", 1);
			writePty(term, buf, len);
			break;

		case Expose:
			/* 画面を再描画する */
			redraw();
			break;

		case ConfigureNotify:
			/* ウィンドウサイズ変更 */
			e = (XConfigureEvent *)&event;
			setWinSize(term, (e->height - 10) / chary,
					(e->width - 20) / charx,
					e->width, e->height);
			break;
		}
	}
}

void
redraw(void)
{
	XGlyphInfo ginfo;
	XWindowAttributes wattr;
	Line *line;
	int x, y;
	int index;
	int i;

	/* 画面をクリア */
	XGetWindowAttributes(disp, win->window, &wattr);
	XClearArea(disp, win->window, 0, 0, wattr.width, wattr.height, False);

	/* 端末の内容をウィンドウに表示 */
	for (i = 0; (line = getLine(term, i)); i++)
		drawLine(line, 10, (i + 1) * chary);

	/* カーソルの位置を取得 */
	line = getLine(term, term->cy);
	index = getCharCnt(line, term->cx).index;
	XftTextExtents32(disp, font, line->str, index, &ginfo);

	/* カーソルを描く */
	x = ginfo.xOff + 10;
	y = term->cy * chary;
	XSetForeground(disp, win->gc, term->palette[deffg]);
	XDrawRectangle(disp, win->window, win->gc, x, y + chary/4, charx, chary);

	/* スポット位置 */
	win->spot.x = x;
	win->spot.y = y + chary;
	XSetICValues(win->xic, XNPreeditAttributes, win->spotlist, NULL);

	XFlush(disp);
}

void
drawLine(Line *line, int xoff, int yoff)
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
		if (bg != defbg) {
			XSetForeground(disp, win->gc, term->palette[bg]);
			XFillRectangle(disp, win->window, win->gc,
					x, yoff - chary * 0.8, width, chary);
		}

		/* 文字 */
		c = term->palette[fg];
		xc.color.red   =   RED(c) << 8;
		xc.color.green = GREEN(c) << 8;
		xc.color.blue  =  BLUE(c) << 8;
		xc.color.alpha = 0xffff;
		XftDrawString32(win->draw, &xc, font, x, y, str, len);

		/* 後処理 */
		if (line->attr[current] & ULINE) { /* 下線 */
			XSetForeground(disp, win->gc, term->palette[fg]);
			XDrawLine(disp, win->window, win->gc, x, yoff, x + width, yoff);
		}
	}
}

Win *
openWindow(void)
{
	Win *win = xmalloc(sizeof(Win));

	/* ウィンドウ作成 */
	win->window = XCreateSimpleWindow(
			disp,
			DefaultRootWindow(disp),
			10, 10, 800, 600, 1,
			term->palette[deffg],
			term->palette[defbg]);

	/* プロパティ */
	hint->res_name = "minty";
	hint->res_class = "minty";
	XSetClassHint(disp, win->window, hint);

	/* イベントマスク */
	XSelectInput(disp, win->window,
			ExposureMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask);

	/* 描画の準備 */
	win->gc = XCreateGC(disp, win->window, 0, NULL);
	win->draw = XftDrawCreate(disp, win->window, visual, cmap);
	XSetForeground(disp, win->gc, term->palette[deffg]);
	XSetBackground(disp, win->gc, term->palette[defbg]);

	/* XIC */
	win->xic = xim ? xicOpen(win) : NULL;
	win->spotlist = XVaCreateNestedList(0,
			XNSpotLocation, &win->spot,
			NULL);

	/* ウィンドウを表示 */
	XMapWindow(disp, win->window);
	XFlush(disp);

	return win;
}

void
closeWindow(Win *win)
{
	XFree(win->spotlist);
	if (win->xic)
		XDestroyIC(win->xic);
	XftDrawDestroy(win->draw);
	XFreeGC(disp, win->gc);
	XDestroyWindow(disp, win->window);
	free(win);
}
