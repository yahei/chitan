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
Win *win;
XClassHint *hint;
Term *term;
int charx, chary;

static void init(void);
static void run(void);
static void fin(void);
static void procXEvent(void);
static void redraw(void);
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
	XFree(hint);
	hint = NULL;
	closeWindow(win);
	win = NULL;
	closeTerm(term);
	term = NULL;
	XftFontClose(disp, font);
	FcFini();
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
		switch (event.type) {
		case KeyPress:
			/* 端末に文字を送る */
			len = XLookupString(&event.xkey, buf, sizeof(buf), NULL, NULL);
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
	XftColor xc;
	Color c;
	Line *line;
	int current, next;
	int index;
	int i;

	/* 画面をクリア */
	XGetWindowAttributes(disp, win->window, &wattr);
	XClearArea(disp, win->window, 0, 0, wattr.width, wattr.height, False);

	/* 端末の内容をウィンドウに表示 */
	for (i = 0; (line = getLine(term, i)); i++) {
		/* 同じSGRの文字列ごとにまとめて書く */
		for (current = 0; line->str[current] != L'\0'; current = next) {
			next = findNextSGR(line, current);
			XftTextExtents32(disp, font, line->str, current, &ginfo);

			if (line->attr[current] & BOLD) { /* 太字 */
				c = term->palette[line->fg[current] +
					(line->fg[current] < 8 ? 8 : 0)];
				xc.color.red   = RED(c)   << 8;
				xc.color.green = GREEN(c) << 8;
				xc.color.blue  = BLUE(c)  << 8;
				xc.color.alpha = 0xffff;
				XftDrawString32(win->draw, &xc, font, ginfo.xOff + 10,
						(i + 1) * chary, line->str + current,
						next - current);
			}
			else { /* 通常 */
				c = term->palette[line->fg[current]];
				xc.color.red   = RED(c)   << 8;
				xc.color.green = GREEN(c) << 8;
				xc.color.blue  = BLUE(c)  << 8;
				xc.color.alpha = 0xffff;
				XftDrawString32(win->draw, &xc, font, ginfo.xOff + 10,
						(i + 1) * chary, line->str + current,
						next - current);
			}
		}
	}

	/* カーソルの位置を取得 */
	line = getLine(term, term->cy);
	index = getCharCnt(line, term->cx).index;
	XftTextExtents32(disp, font, line->str, index, &ginfo);

	/* カーソルを描く */
	int x = ginfo.xOff + 10;
	int y = term->cy * chary;
	XDrawRectangle(disp, win->window, win->gc, x, y + chary/4, charx, chary);

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
			term->palette[deffg],
			term->palette[defbg]);

	/* プロパティ */
	hint->res_name = "minty";
	hint->res_class = "minty";
	XSetClassHint(disp, win->window, hint);

	/* イベントマスク */
	XSelectInput(disp, win->window,
			ExposureMask | KeyPressMask | StructureNotifyMask);

	/* 描画の準備 */
	win->gc = XCreateGC(disp, win->window, 0, NULL);
	win->draw = XftDrawCreate(disp, win->window, visual, cmap);
	XSetForeground(disp, win->gc, term->palette[deffg]);
	XSetBackground(disp, win->gc, term->palette[defbg]);

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
