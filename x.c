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

int
main(int argc, char *args[])
{
	// Xサーバに接続
	Display *display = XOpenDisplay(NULL);
	if (display == NULL) {
		fprintf(stderr, "Xサーバに接続できませんでした\n");
		exit(1);
	}

	// ウィンドウ作成
	Window window = XCreateSimpleWindow(
			display,
			DefaultRootWindow(display),
			10, 10, 800, 600, 1,
			BlackPixel(display, 0),
			WhitePixel(display, 0));

	// ウィンドウ表示
	printf("open window.\n");
	XMapWindow(display, window);
	printf("opened.\n");

	// ウィンドウ移動
	XMoveWindow(display, window, 10, 10);

	// イベントマスクの設定
	XSelectInput(display, window, ExposureMask | KeyPressMask);

	// 描画の準備
	GC gc = XCreateGC(display, window, 0, NULL);
	XSetForeground(display, gc, BlackPixel(display,0));
	XSetBackground(display, gc, WhitePixel(display,0));

	// 文字描画の準備
	XftFont *font = XftFontOpen(
			display, 0,
			XFT_FAMILY, XftTypeString, "Noto Serif CJK JP",
			XFT_SIZE, XftTypeDouble, 24.0,
			NULL);
	Colormap cmap = DefaultColormap(display, 0);
	XftColor color;
	XftColorAllocName(display, DefaultVisual(display, 0), cmap, "red", &color);
	printf("%lx\n", color.pixel);
	XftDraw *draw = XftDrawCreate(display, window, DefaultVisual(display, 0), cmap);
	char str[64] = "あいうabc";

	// 仮想端末のオープン
	Term *term = newTerm();
	if (!term)
		errExit("newTerm failed.\n");

	// pselectの準備
	fd_set rfds;
	struct timespec timeout, *ptimeout;
	int tfd = getfdTerm(term);
	int xfd = XConnectionNumber(display);

	// イベントループ
	for (;;) {
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
			int last;
			Line *line;
			char *mstr;

			printf("read start.\n");
			if (readpty(term) < 0) {
				/* 終了 */
				printf("exit...");
				sleep(5);
				goto CLOSE;
			}

			/* 表示 */
			last = getlastlineTerm(term);
			for(int i = 0; i <= last; i++) {
				line = getlineTerm(term, i);
				mstr = getmbLine(line);
				printf("%d|%s\n", i, mstr);
			}
			printf("read finish.\n");
		}

		/* ウィンドウのイベント処理 */
		if (FD_ISSET(xfd, &rfds)) {
			while (XPending(display) > 0) {
				XEvent event;
				XNextEvent(display, &event);

				// イベントの種類番号を出力
				printf("%d,", event.type);
				fflush(stdout);

				if (event.type == KeyPress) {
					printf("%d.", event.xkey.keycode);
					fflush(stdout);

					// ESCが押されたら終了する
					if (event.xkey.keycode == 9)
						goto CLOSE;
				}

				// 描画のテスト
				int a= rand()%200;
				int b= rand()%200;
				XDrawLine(display, window, gc, 10, a, 100, b);

				// 文字の描画
				XftDrawStringUtf8(draw, &color, font, 10, 50,
						(FcChar8*)str, strlen(str));
			}
		}
	}
CLOSE:
	printf("\n");

	// 仮想端末のクローズ
	deleteTerm(term);
	term = NULL;

	// XftのClose
	XftFontClose(display, font);
	XftDrawDestroy(draw);

	// Xサーバから切断
	XCloseDisplay(display);

	/* freeしていないメモリの数 */
	if (memcnt != 0)
		fprintf(stderr, "memcnt: %d\n", memcnt);

	return 0;
}
