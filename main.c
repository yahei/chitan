#define _XOPEN_SOURCE 600

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
} Win;

Display *disp;
Visual *visual;
Colormap cmap;
XftFont *font;
XftColor color;
Win *win;
Term *term;

int main(int, char *[]);
void init(void);
void run(void);
void fin(void);
void procXEvent(void);
void redraw(void);
Win *openWindow(void);
void closeWindow(Win *);

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
	/* localeを設定 */
	setlocale(LC_CTYPE, "");

	/* Xサーバーに接続 */
	disp= XOpenDisplay(NULL);
	if (disp== NULL)
		fatal("XOpenDisplay failed.\n");
	visual = DefaultVisual(disp, 0);
	cmap = DefaultColormap(disp, 0);

	/* 文字描画の準備 */
	FcInit();
	font = XftFontOpen(
			disp, 0,
			XFT_FAMILY, XftTypeString, "Noto Serif CJK JP",
			XFT_SIZE, XftTypeDouble, 12.0,
			NULL);
	if (font == NULL)
		fatal("XftFontOpen failed.\n");
	if (XftColorAllocName(disp, visual, cmap, "black", &color) == 0)
		fatal("XftColorAllocName failed.\n");
	
	/* 端末をオープン */
	term = openTerm();
	if (!term)
		errExit("newTerm failed.\n");
	
	/* ウィンドウの作成 */
	win = openWindow();
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
		ptimeout = (timeout.tv_nsec > 0 || timeout.tv_nsec > 0) ?
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
	closeWindow(win);
	win = NULL;
	closeTerm(term);
	term = NULL;
	XftColorFree(disp, visual, cmap, &color);
	XftFontClose(disp, font);
	FcFini();
	XCloseDisplay(disp);
}

void
procXEvent(void)
{
	XEvent event;
	char buf[256];
	int len;

	while (XPending(disp) > 0) {
		XNextEvent(disp, &event);
		switch (event.type) {
		case KeyPress:
			/* 端末に文字を送る */
			len = XLookupString(&event.xkey, buf, sizeof(buf), NULL, NULL);
			printf("(%s)", buf);
			fflush(stdout);
			writePty(term, buf, len);
			break;
		case Expose:
			redraw();
			break;
		}
	}
}

void
redraw(void)
{
	int last;
	Line *line;
	int row;
	XGlyphInfo ginfo;
	XWindowAttributes wattr;
	int lineh;
	int drawy;

	/* 端末の内容を自分の標準出力に表示 */
	last = term->lastline;
	for (int i = 0; i <= last; i++) {
		line = term->lines[i];
		printf("%d|%ls|\n", i, (wchar_t *)line->str);
	}

	/* テキストの高さや横幅を取得 */
	XftTextExtents32(disp, font, (utf32 *)L"pl", 2, &ginfo);
	lineh = ginfo.height * 1.25;

	last = term->lastline;
	line = term->lines[last];
	XftTextExtents32(disp, font, line->str, term->cursor, &ginfo);

	/* 画面をクリア */
	XGetWindowAttributes(disp, win->window, &wattr);
	XClearArea(disp, win->window, 0, 0, wattr.width, wattr.height, False);

	/* 端末の内容をウィンドウに表示 */
	for (drawy = (int)(wattr.height / lineh), row = term->lastline;
			row >= 0 && drawy >= 0;
			row--, drawy--) {
		line = term->lines[row];
		XftDrawString32(win->draw, &color, font, 10, drawy * lineh,
				line->str, utf32slen(line->str));
	}

	/* カーソルを描く */
	int x = ginfo.xOff + 10;
	int y = (int)(wattr.height / lineh - 1) * lineh;
	XDrawRectangle(disp, win->window, win->gc, x, y + lineh/4, lineh/2, lineh);

	XFlush(disp);
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
			BlackPixel(disp, 0),
			WhitePixel(disp, 0));

	/* イベントマスク */
	XSelectInput(disp, win->window, ExposureMask | KeyPressMask);

	/* 描画の準備 */
	win->gc = XCreateGC(disp, win->window, 0, NULL);
	win->draw = XftDrawCreate(disp, win->window, visual, cmap);
	XSetForeground(disp, win->gc, BlackPixel(disp,0));
	XSetBackground(disp, win->gc, WhitePixel(disp,0));

	/* ウィンドウを表示 */
	XMapWindow(disp, win->window);
	XFlush(disp);

	return win;
}

void
closeWindow(Win *win)
{
	XftDrawDestroy(win->draw);
	XFreeGC(disp, win->gc);
	XDestroyWindow(disp, win->window);
	free(win);
}
