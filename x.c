#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include "tty.h"

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
			0);
	Colormap cmap = DefaultColormap(display, 0);
	XftColor color;
	XftColorAllocName(display, DefaultVisual(display, 0), cmap, "red", &color);
	printf("%x\n", color.pixel);
	XftDraw *draw = XftDrawCreate(display, window, DefaultVisual(display, 0), cmap);
	char str[64] = "あいうabc";

	// 仮想端末のオープン
	Term *term = newTerm();
	if (!term)
		fprintf(stderr, "openterm failed : %s\n", strerror(errno));

	// イベントループ
	for (;;) {
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
				break;
		}

		// 描画のテスト
		int a= rand()%200;
		int b= rand()%200;
		XDrawLine(display, window, gc, 10, a, 100, b);

		// 文字の描画
		XftDrawStringUtf8(draw, &color, font, 10, 50, (FcChar8*)str, strlen(str));
	}
	printf("\n");

	// 仮想端末のクローズ
	deleteTerm(term);
	term = NULL;

	// XftのClose
	XftFontClose(display, font);
	XftDrawDestroy(draw);

	// Xサーバから切断
	XCloseDisplay(display);

	return 0;
}
