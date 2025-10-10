#include <sys/select.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wchar.h>
#include <X11/Xlib.h>

#include "term.h"
#include "font.h"
#include "util.h"

#define CHOOSE(a, b, c) (timespeccmp((a), (b), c) ? (a) : (b))
#define BLEND_COLOR(c1, a1, c2, a2) (\
		((int)(  RED(c1) * (a1) +   RED(c2) * (a2)) << 16) +\
		((int)(GREEN(c1) * (a1) + GREEN(c2) * (a2)) <<  8) +\
		((int)( BLUE(c1) * (a1) +  BLUE(c2) * (a2)) <<  0))
#define BELLCOLOR(color) (win->timer_lit[BELL_TIMER] ?\
		BLEND_COLOR(color, 0.85, 0xffffffff, 0.15) : color)

enum timer_names { BELL_TIMER, BLINK_TIMER, RAPID_TIMER, CARET_TIMER, TIMER_NUM };

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
	int redraw_flag;
	struct timespec timers[TIMER_NUM];
	char timer_active[TIMER_NUM], timer_lit[TIMER_NUM];
	char focus;
	struct Selection{
		int aline, acol, bline, bcol;
		int rect, altbuf, dragging;
		char *str, *clip;
	} selection;
} Win;

static Display *disp;
static Visual *visual;
static Colormap cmap;
static XFont *xfont;
static XIM xim;
static Win *win;
static struct timespec now;

static void init(void);
static void run(void);
static void fin(void);

/* Win */
static Win *openWindow(void);
static void closeWindow(Win *);
static void procXEvent(Win *);
static void mouseEvent(Win *, XEvent *);
static void copySelection(Win *);
static void procKeyPress(Win *, XEvent, int);
static void redraw(Win *);
static void drawLine(Win *, Line *, int, int, int, int, int);
static void drawCursor(Win *, Line *, int, int, int, int, int);
static void drawSelection(Win *, struct Selection *);
static void drawLineRev(Win *, Line *, int, int, int, int, int);
static void bell(void *);

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
	xfont = openFont(disp, "monospace", 12.0);
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
	struct timespec timeout = { 0, 1000 }, nexttime;
	static const struct timespec duration[] = {
		[BELL_TIMER]  = { 0, 150 * 1000 * 1000 },
		[BLINK_TIMER] = { 0, 800 * 1000 * 1000 },
		[RAPID_TIMER] = { 0, 200 * 1000 * 1000 },
		[CARET_TIMER] = { 0, 500 * 1000 * 1000 },
	};
	fd_set rfds;
	int tfd = win->term->master;
	int xfd = XConnectionNumber(disp);
	int i;

	while (1) {
		/* ファイルディスクリプタの監視 */
		FD_ZERO(&rfds);
		FD_SET(tfd, &rfds);
		FD_SET(xfd, &rfds);
		if (pselect(MAX(tfd, xfd) + 1, &rfds, NULL, NULL, &timeout, NULL) < 0) {
			if (errno == EINTR)
				fprintf(stderr, "signal.\n");
			else
				errExit("pselect failed.\n");
		}
		clock_gettime(CLOCK_MONOTONIC, &now);

		/* ウィンドウのイベント処理 */
		if (XPending(disp))
			procXEvent(win);

		/* 端末のread */
		if (0 < readPty(win->term)) {
			win->redraw_flag = 1;
			/* IMEスポット移動 */
			if (win->ime.xic) {
				win->ime.spot.x = win->xpad + win->term->cx * xfont->cw;
				win->ime.spot.y = win->ypad + win->term->cy * xfont->ch + xfont->ascent;
				XSetICValues(win->ime.xic, XNPreeditAttributes, win->ime.spotlist, NULL);
			}
		} else if (errno == EIO) {
			return;
		}

		/* フォーカスが外れているときはキャレットのタイマーを止める */
		win->timer_active[CARET_TIMER] = win->focus && (!win->term->ctype || win->term->ctype % 2);

		/* タイムアウトの設定 */
		nexttime = (struct timespec){ 1 << 16, 0 };
		timespecadd(&now, &nexttime, &nexttime);
		for (i = 0; i < TIMER_NUM; i++) {
			if (!win->timer_active[i])
				continue;
			if (timespeccmp(&win->timers[i], &now, <=)) {
				win->timer_lit[i] = !win->timer_lit[i];
				win->redraw_flag = 1;
				timespecadd(&win->timers[i], &duration[i], &win->timers[i]);
				if (timespeccmp(&win->timers[i], &now, <=))
					timespecadd(&now, &duration[i], &win->timers[i]);
			}
			nexttime = *CHOOSE(CHOOSE(&win->timers[i], &nexttime, <), &now, >);
		}
		timespecsub(&nexttime, &now, &timeout);

		/* ベルは繰り返さない */
		if (win->timer_lit[BELL_TIMER] == 0)
			win->timer_active[BELL_TIMER] = 0;

		/* 再描画 */
		if (win->redraw_flag) {
			redraw(win);
			win->redraw_flag = 0;
		}
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

Win *
openWindow(void)
{
	Win *win = xmalloc(sizeof(Win));

	*win = (Win) { .width = 800, .height = 600};
	win->xpad = win->ypad = xfont->cw / 2;

	/* 端末をオープン */
	win->term = openTerm();
	if (!win->term)
		errExit("newTerm failed.\n");
	win->term->bell = bell;

	/* ウィンドウ作成 */
	win->window = XCreateSimpleWindow(
			disp, DefaultRootWindow(disp),
			0, 0, win->width, win->height, 1,
			win->term->palette[deffg],
			win->term->palette[defbg]);
	win->col = (win->width - win->xpad * 2) / xfont->cw;
	win->row = (win->height - win->ypad * 2) / xfont->ch;
	setWinSize(win->term, win->row, win->col, win->width, win->height);

	/* プロパティ */
	win->hint = XAllocClassHint();
	win->hint->res_name = "chitan";
	win->hint->res_class = "chitan";
	XSetClassHint(disp, win->window, win->hint);

	/* イベントマスク */
	XSelectInput(disp, win->window,
			FocusChangeMask | ExposureMask | StructureNotifyMask |
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
	XSync(disp, False);

	return win;
}

void
closeWindow(Win *win)
{
	free(win->selection.str);
	free(win->selection.clip);
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
	XSelectionRequestEvent *sre;
	int mx, my, state, mb = 0;
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
			/* 擬似端末に通知 */
			state = event.xbutton.state;
			mb = event.xbutton.button;
			if ((mb != 1 && mb != 3) || (state & ~(ShiftMask | Mod1Mask)) ||
					(win->term->sb == &win->term->alt && !(state & ShiftMask))) {
				mouseEvent(win, &event);
				break;
			}
			/* 範囲選択のドラッグを開始 */
			win->selection.rect = 0 < (state & Mod1Mask);
			win->selection.altbuf = win->term->sb == &win->term->alt;
			win->selection.dragging = 1;

		case MotionNotify:
			/* 疑似端末に通知 */
			if (!win->selection.dragging) {
				mouseEvent(win, &event);
				break;
			}

			/* ここからPress/Motion共通の処理 */
			/* 範囲選択の終点を設定 */
			mx = (event.xbutton.x - win->xpad) / xfont->cw;
			my = (event.xbutton.y - win->ypad) / xfont->ch;
			win->selection.bcol = mx;
			win->selection.bline = my + win->term->sb->firstline;
			/* 範囲選択の始点を設定 */
			if (mb == 1) {
				win->selection.acol = mx;
				win->selection.aline = my + win->term->sb->firstline;
			}
			win->redraw_flag = 1;
			break;

		case ButtonRelease:
			/* 疑似端末に通知 */
			if (!win->selection.dragging) {
				mouseEvent(win, &event);
				break;
			}
			/* 範囲選択のドラッグを終了 */
			win->selection.dragging = 0;
			copySelection(win);
			break;

		case Expose:
			/* 画面を再描画する */
			win->redraw_flag = 1;
			break;

		case ConfigureNotify:
			/* ウィンドウサイズ変更 */
			e = (XConfigureEvent *)&event;
			if (win->width == e->width && win->height == e->height)
				break;
			win->width = e->width;
			win->height = e->height;
			win->col = (win->width - win->xpad * 2) / xfont->cw;
			win->row = (win->height - win->ypad * 2) / xfont->ch;
			setWinSize(win->term, win->row, win->col, win->width, win->height);
			break;

		case SelectionRequest:
			/* 貼り付ける文字列を送る */
			if (!win->selection.clip || strlen(win->selection.clip) == 0)
				break;
			sre = &event.xselectionrequest;
			if (sre->property == None)
				sre->property = sre->target;
			XChangeProperty(disp, sre->requestor, sre->property,
					atoms[UTF8_STRING], 8, PropModeReplace,
					(unsigned char *)win->selection.clip,
					strlen(win->selection.clip));
			XSelectionEvent se = { SelectionNotify, 0, True, disp,
				sre->requestor, sre->selection, sre->target,
				sre->property, sre->time };
			XSendEvent(disp, event.xselectionrequest.requestor, False, 0, (XEvent *)&se);
			break;

		case SelectionNotify:
			/* 貼り付ける文字列が届いた */
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

		case FocusIn:
			win->focus = 1;
			win->redraw_flag = 1;
			break;
		case FocusOut:
			win->focus = 0;
			win->redraw_flag = 1;
			break;
		}
	}
}

void
mouseEvent(Win *win, XEvent *event)
{
	int mb, state = event->xbutton.state;

	if (event->type == MotionNotify) {
		mb = (state & Button1Mask ?  0 :
		      state & Button2Mask ?  1 :
		      state & Button3Mask ?  2 : 3) + MOVE;
	} else {
		mb = event->xbutton.button;
		mb = BETWEEN(mb, 1, 4)  ? (mb - 1) :
		     BETWEEN(mb, 4, 8)  ? (mb - 4) + WHEEL :
		     BETWEEN(mb, 8, 12) ? (mb - 8) + OTHER : 3;
	}
	mb += state & ShiftMask   ? SHIFT : 0;
	mb += state & Mod1Mask    ? ALT   : 0;
	mb += state & ControlMask ? CTRL  : 0;
	reportMouse(win->term, mb, event->type == ButtonRelease,
			(event->xbutton.x - win->xpad) / xfont->cw + 1,
			(event->xbutton.y - win->ypad) / xfont->ch + 1);
}

void
copySelection(Win *win)
{
	int len = 256;
	char32_t *cp, *copy = xmalloc(len * sizeof(copy[0]));
	int firstline = MIN(win->selection.aline, win->selection.bline);
	int lastline  = MAX(win->selection.aline, win->selection.bline);
	int left      = MIN(win->selection.acol,  win->selection.bcol);
	int right     = MAX(win->selection.acol,  win->selection.bcol);
	Line *line;
	int i, l, r;

	copy[0] = L'\0';

	/* 選択範囲の文字列(UTF32)を読み出してコピー */
	for(i = firstline; i <= lastline; i++) {
		if (!(line = getLine(win->term, i - win->term->sb->firstline)))
			continue;

		if (win->selection.rect) {
			l = MIN(getCharCnt(line->str,  left).index, u32slen(line->str));
			r = MIN(getCharCnt(line->str, right).index, u32slen(line->str));
		} else {
			l = (i != firstline) ? 0 :
				MIN(getCharCnt(line->str, left).index, u32slen(line->str));
			r = (i != lastline) ? u32slen(line->str) + 1 :
				MIN(getCharCnt(line->str, right).index, u32slen(line->str));
		}

		while (len < u32slen(copy) + r - l + 2) {
			len += 256;
			copy = xrealloc(copy, len * sizeof(copy[0]));
		}
		cp = copy + u32slen(copy);
		wcsncpy((wchar_t *)cp, (wchar_t *)line->str + l, r - l);
		cp[r - l] = L'\0';
		if (i < lastline)
			wcscat((wchar_t *)copy, L"\n");
	}

	/* UTF8に変換して保存 */
	win->selection.str = xrealloc(win->selection.str, len * 4);
	wcstombs(win->selection.str, (wchar_t *)copy, len * 4);

	free(copy);
}

void
procKeyPress(Win *win, XEvent event, int bufsize)
{
	struct Key { int symbol; char *normal; char *app; } keys[] = {
		{ XK_Up,        "\e[A",         "\eOA" },
		{ XK_Down,      "\e[B",         "\eOB" },
		{ XK_Right,     "\e[C",         "\eOC" },
		{ XK_Left,      "\e[D",         "\eOD" },
		{ XK_Home,      "\e[H",         "\eOH" },
		{ XK_End,       "\e[F",         "\eOF" },
		{ XK_Page_Up,   "\e[5~",        "\e[5~" },
		{ XK_Page_Down, "\e[6~",        "\e[6~" },
		{ XK_Insert,    "\e[2~",        "\e[2~" },
		{ XK_F1,        "\eOP",         "\eOP" },
		{ XK_F2,        "\eOQ",         "\eOQ" },
		{ XK_F3,        "\eOR",         "\eOR" },
		{ XK_F4,        "\eOS",         "\eOS" },
		{ XK_F5,        "\e[15~",       "\e[15~" },
		{ XK_F6,        "\e[17~",       "\e[17~" },
		{ XK_F7,        "\e[18~",       "\e[18~" },
		{ XK_F8,        "\e[19~",       "\e[19~" },
		{ XK_F9,        "\e[20~",       "\e[20~" },
		{ XK_F10,       "\e[21~",       "\e[21~" },
		{ XK_F11,       "\e[23~",       "\e[23~" },
		{ XK_F12,       "\e[24~",       "\e[24~" },
		{ XK_VoidSymbol }
	}, *key;
	char buf[bufsize], *str;
	int len;
	KeySym keysym;
	Status status = XLookupChars;

	/* 点滅をリセット */
	win->timer_lit[CARET_TIMER] = 1;
	win->timers[CARET_TIMER] = now;

	/* 入力文字列を取得 */
	len = win->ime.xic ?
		Xutf8LookupString(win->ime.xic, &event.xkey, buf, bufsize, &keysym, &status) :
		XLookupString(&event.xkey, buf, bufsize, &keysym, NULL);
	if (status == XBufferOverflow)
		return procKeyPress(win, event, len);

	/* IMEの確定っぽい場合(コールバックがすぐ来ない場合があるため) */
	if (1 < strlen(buf))
		PUT_NUL(win->ime.peline, 0);

	/* C-S-cでコピー */
	if (keysym == XK_C && event.xkey.state & ControlMask) {
		XSetSelectionOwner(disp, atoms[CLIPBOARD], win->window, event.xkey.time);
		win->selection.clip = xrealloc(win->selection.clip, strlen(win->selection.str) + 1);
		strcpy(win->selection.clip, win->selection.str);
		return;
	}

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
		/* カーソルキー等を送る */
		for (key = keys; key->symbol != XK_VoidSymbol; key++) {
			if (key->symbol != keysym)
				continue;
			str = win->term->dec[1] < 2 ? key->normal : key->app;
			writePty(win->term, str, strlen(str));
			break;
		}
	}
}

void
redraw(Win *win)
{
	XWindowAttributes wattr;
	Line *line;
	int pepos, pewidth, pecaretpos;
	int i;

	/* 点滅中フラグをクリア */
	win->timer_active[BLINK_TIMER] = win->timer_active[RAPID_TIMER] = 0;

	/* 画面をクリア */
	XGetWindowAttributes(disp, win->window, &wattr);
	XSetWindowBackground(disp, win->window, BELLCOLOR(win->term->palette[defbg]));
	XClearArea(disp, win->window, 0, 0, wattr.width, wattr.height, False);

	/* 端末の内容をウィンドウに表示 */
	for (i = 0; (line = getLine(win->term, i)); i++)
		drawLine(win, line, i, 0, win->col, win->xpad, win->ypad);

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
		drawLine(win, win->ime.peline, win->term->cy, 0, pewidth,
				win->xpad + pepos * xfont->cw, win->ypad);
		drawCursor(win, win->ime.peline, win->term->cy, pepos + pecaretpos,
				win->xpad, win->ypad, 6);
	} else if (1 <= win->term->dec[25] && win->term->cx < win->col) {
		/* カーソルの描画 */
		line = getLine(win->term, win->term->cy);
		drawCursor(win, line, win->term->cy, win->term->cx,
				win->xpad, win->ypad, win->term->ctype);
	}

	/* 選択範囲を書く */
	if (win->selection.altbuf == (win->term->sb == &win->term->alt))
		drawSelection(win, &win->selection);

	XSync(disp, False);
}

void
drawLine(Win *win, Line *line, int row, int col, int width, int xoff, int yoff)
{
	int next, i = getCharCnt(line->str, col).index;
	int x, y, w;
	int attr, fg, bg;
	XftColor xc;
	Color c;

	if (width <= col || line->str[i] == L'\0')
		return;

	/* 同じ属性の文字はまとめて処理する */
	next = MIN(findNextSGR(line, i), width);
	drawLine(win, line, row, col + u32swidth(&line->str[i], next - i), width, xoff, yoff);

	/* 点滅 */
	if (line->attr[i] & BLINK)
		win->timer_active[BLINK_TIMER] = 1;
	if (line->attr[i] & RAPID)
		win->timer_active[RAPID_TIMER] = 1;
	if (!(((line->attr[i] & BLINK) && win->timer_lit[BLINK_TIMER]) ||
	      ((line->attr[i] & RAPID) && win->timer_lit[RAPID_TIMER]) ||
	      (!(line->attr[i] & BLINK) && !(line->attr[i] & RAPID))))
		return;

	/* 座標 */
	x = xoff + col * xfont->cw;
	y = yoff + row * xfont->ch;
	w = xfont->cw * u32swidth(&line->str[i], next - i);

	/* 前処理 */
	if (line->attr[i] & NEGA) { /* 反転 */
		fg = line->bg[i];
		bg = line->fg[i];
	} else {
		fg = line->fg[i];
		bg = line->bg[i];
	}
	if (line->attr[i] & BOLD) /* 太字 */
		fg = fg < 8 ? fg + 8 : fg;
	c = win->term->palette[fg]; /* 色を取得 */
	if (line->attr[i] & FAINT) /* 細字 */
		c = BLEND_COLOR(c, 0.6, win->term->palette[bg], 0.4);

	/* 色をXftColorに変換 */
	xc.color.red   =   RED(c) << 8;
	xc.color.green = GREEN(c) << 8;
	xc.color.blue  =  BLUE(c) << 8;
	xc.color.alpha = 0xffff;

	/* 背景を塗る */
	XSetForeground(disp, win->gc, BELLCOLOR(win->term->palette[bg]));
	XFillRectangle(disp, win->window, win->gc, x, y, w, xfont->ch);

	y += xfont->ascent;

	/* 文字を書く */
	attr = FONT_NONE;
	attr |= line->attr[i] & BOLD   ? FONT_BOLD   : FONT_NONE;
	attr |= line->attr[i] & ITALIC ? FONT_ITALIC : FONT_NONE;
	drawXFontString(win->draw, &xc, xfont, attr, x, y, &line->str[i], next - i);

	/* 後処理 */
	if (line->attr[i] & ULINE) { /* 下線 */
		XSetForeground(disp, win->gc, win->term->palette[fg]);
		XDrawLine(disp, win->window, win->gc, x, y + 1, x + w, y + 1);
	}
}

void
drawCursor(Win *win, Line *line, int row, int col, int xoff, int yoff, int type)
{
	const int index = getCharCnt(line->str, col).index;
	char32_t *c = index < u32slen(line->str) ? &line->str[index] : (char32_t *)L" ";
	const int x = xoff + col * xfont->cw;
	const int y = yoff + row * xfont->ch;
	const int width = xfont->cw * u32swidth(c, 1) - 1;
	int attr;
	Line cursor;

	/* 点滅 */
	if (win->focus && (!type || type % 2) && (win->timer_lit[CARET_TIMER]))
		return;

	switch (type) {
	default: /* ブロック */
	case 0:
	case 1:
	case 2:
		if (win->focus) {
			attr = index < u32slen(line->str) ? line->attr[index] : 0;
			cursor = (Line){c, &attr, &defbg, &deffg};
			drawLine(win, &cursor, 0, 0, 1, x, y);
		} else {
			XSetForeground(disp, win->gc, BELLCOLOR(win->term->palette[deffg]));
			XDrawRectangle(disp, win->window, win->gc, x, y, width, xfont->ch - 1);
		}
		break;
	case 3: /* 下線 */
	case 4:
		XFillRectangle(disp, win->window, win->gc, x, y + 1 + xfont->ascent, width, xfont->ch * 0.1);
		break;
	case 5: /* 縦線 */
	case 6:
		XFillRectangle(disp, win->window, win->gc, x - 1, y, xfont->ch * 0.1, xfont->ch);
		break;
	}
}

void
drawSelection(Win *win, struct Selection *sel)
{
	const int s = MIN(sel->aline, sel->bline) - win->term->sb->firstline;
	const int e = MAX(sel->aline, sel->bline) - win->term->sb->firstline;
	int i;

#define DRAW(n, a, b)   drawLineRev(win, getLine(win->term, n), n, a, b, win->xpad, win->ypad)
	if (sel->rect) {
		/* 矩形選択 */
		for (i = MAX(s, 0); i < MIN(e + 1, win->row); i++)
			DRAW(i, sel->acol, sel->bcol);
	} else {
		/* 通常 */
		if (sel->aline == sel->bline) {
			DRAW(s, sel->acol, sel->bcol);
		} else {
			DRAW(s, (sel->aline < sel->bline ? sel->acol : sel->bcol), win->col + 1);
			DRAW(e, 0, (sel->aline < sel->bline ? sel->bcol : sel->acol));
			for (i = MAX(s + 1, 0); i < MIN(e, win->row); i++)
				DRAW(i, 0, win->col + 1);
		}
	}
#undef DRAW
}

void
drawLineRev(Win *win, Line *line, int row, int col1, int col2, int xpad, int ypad)
{
	const Color c = win->term->palette[defbg];
	XftColor xc = { 0, { RED(c) << 8, GREEN(c) << 8, BLUE(c) << 8, 0xffff } };
	int li, ri;
	int xoff, yoff, len;

	if (!line)
		return;

	li = MIN(getCharCnt(line->str, MIN(col1, col2)).index, u32slen(line->str));
	ri = MIN(getCharCnt(line->str, MAX(col1, col2)).index, u32slen(line->str));

	xoff = xpad + getCharCnt(line->str, MIN(col1, col2)).col * xfont->cw;
	yoff = ypad + row * xfont->ch;
	len = MIN(u32slen(line->str + li), ri - li);

	XSetForeground(disp, win->gc, BELLCOLOR(win->term->palette[deffg]));
	XFillRectangle(disp, win->window, win->gc, xoff, yoff,
			u32swidth(line->str + li, len) * xfont->cw, xfont->ch);
	drawXFontString(win->draw, &xc, xfont, 0, xoff, yoff + xfont->ascent,
			line->str + li, len);
}

void
bell(void *term)
{
	win->timer_active[BELL_TIMER] = 1;
	win->timers[BELL_TIMER] = now;
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
