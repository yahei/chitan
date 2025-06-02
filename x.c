#define _XOPEN_SOURCE 600

#include <sys/select.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include "tty.h"
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
	
	/* ウィンドウの作成 */
	win = openWindow();

	/* 仮想端末のオープン */
	term = newTerm();
	if (!term)
		errExit("newTerm failed.\n");
}

void
run(void)
{
	fd_set rfds;
	struct timespec timeout, *ptimeout;
	int tfd = getfdTerm(term);
	int xfd = XConnectionNumber(disp);

	while(1) {
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

		/* 疑似端末のread */
		if (FD_ISSET(tfd, &rfds)) {
			if (readptyTerm(term) < 0) {
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
	deleteTerm(term);
	term = NULL;
	closeWindow(win);
	win = NULL;
	XftColorFree(disp, visual, cmap, &color);
	XftFontClose(disp, font);
	FcFini();
	XCloseDisplay(disp);
}

void
procXEvent(void)
{
	XEvent event;

	while (XPending(disp) > 0) {
		XNextEvent(disp, &event);

		if (event.type == KeyPress) {
			unsigned char ks = XLookupKeysym(&event.xkey, 0);
			switch(ks) {
			case '\r' :
				printf("[return](%d)\n", ks);
				break;
			default:
				printf("%c(%d).", ks, ks);
			}
			fflush(stdout);

			// ESCが押されたら終了する
			if (event.xkey.keycode == 9) {
				printf("\n");
				errExit("ESCで終了\n");
			}

			/* Termに文字を送る */
			writeptyTerm(term, (char *)&ks, 1);
		}
	}
}

void
redraw(void)
{
	int last;
	Line *line;
	char *mstr;
	int row;

	/* ターミナルの内容を自分の標準出力に表示 */
	last = getlastlineTerm(term);
	for(int i = 0; i <= last; i++) {
		line = getlineTerm(term, i);
		mstr = getmbLine(line);
		printf("%d|%s\n", i, mstr);
	}

	/* ターミナルの内容をウィンドウに表示 */
	for (row = getlastlineTerm(term); row >= 0; row--) {
		int y = row + 1;
		line = getlineTerm(term, row);
		mstr = getmbLine(line);
		XftDrawStringUtf8(win->draw, &color, font, 10, y * 30,
				(FcChar8*)mstr, strlen(mstr));
	}
	XSync(disp, False);
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
	XMoveWindow(disp, win->window, 10, 10);
	XSync(disp, False);

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
