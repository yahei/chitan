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
		((int)(ALPHA(c1) * (a1) + ALPHA(c2) * (a2)) << 24) +\
		((int)(  RED(c1) * (a1) +   RED(c2) * (a2)) << 16) +\
		((int)(GREEN(c1) * (a1) + GREEN(c2) * (a2)) <<  8) +\
		((int)( BLUE(c1) * (a1) +  BLUE(c2) * (a2)) <<  0))
#define BELLCOLOR(color) (pane->timer_lit[BELL_TIMER] ?\
		BLEND_COLOR(color, 0.85, 0xffffffff, 0.15) : color)
#define SCROLLMAX(sb)   (sb->firstline - MAX(sb->totallines - sb->maxlines, 0))

enum timer_names { BELL_TIMER, BLINK_TIMER, RAPID_TIMER, CARET_TIMER, TIMER_NUM };

typedef struct IME {
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
} IME;

typedef struct Pane {
	Pixmap pixmap;
	Drawable d;
	unsigned int depth;
	GC gc;
	XftDraw *draw;
	int width, height, xpad, ypad;
	char focus, redraw_flag;
	Term *term;
	int scr, prevfst;
	struct ScreenBuffer *prevbuf;
	struct Selection {
		int aline, acol, bline, bcol;
		int rect, altbuf, dragging;
		char *primary, *clip;
	} selection;
	struct timespec timers[TIMER_NUM];
	char timer_active[TIMER_NUM];
	char timer_lit[TIMER_NUM];
} Pane;

typedef struct Win {
	Window window;
	XClassHint *hint;
	XSetWindowAttributes attr;
	IME ime;
	GC gc;
	XftDraw *draw;
	int width, height;
	Pane *pane;
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

/* Pane */
static Pane *createPane(Display *, Drawable, int, int, int);
static void destroyPane(Pane *);
static void setPaneSize(Pane *, int, int);

/* Win */
static Win *openWindow(void);
static void closeWindow(Win *);
static char handleXEvent(Win *);
static void mouseEvent(Pane *, XEvent *);
static void copySelection(Pane *, char **);
static char keyPressEvent(Win *, XEvent, int);
static void redraw(Win *);
static void drawPane(Pane *, const IME *);
static void drawLine(Pane *, Line *, int, int, int, int);
static void drawCursor(Pane *, Line *, int, int, int);
static void drawSelection(Pane *, struct Selection *);
static void drawLineRev(Pane *, Line *, int, int, int);
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
	XVisualInfo vinfo;
	char **names;
	int i;

	/* localeを設定 */
	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers("");

	/* Xサーバーに接続 */
	disp= XOpenDisplay(NULL);
	if (disp== NULL)
		fatal("XOpenDisplay failed.\n");
	XMatchVisualInfo(disp, XDefaultScreen(disp), 32, TrueColor, &vinfo);
	visual = vinfo.visual;
	cmap = XCreateColormap(disp, DefaultRootWindow(disp), visual, None);

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
	Pane *pane = win->pane;
	struct timespec timeout = { 0, 1000 }, nexttime;
	static const struct timespec duration[] = {
		[BELL_TIMER]  = { 0, 150 * 1000 * 1000 },
		[BLINK_TIMER] = { 0, 800 * 1000 * 1000 },
		[RAPID_TIMER] = { 0, 200 * 1000 * 1000 },
		[CARET_TIMER] = { 0, 500 * 1000 * 1000 },
	};
	fd_set rfds;
	int tfd = pane->term->master;
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
		if (handleXEvent(win) < 0)
			return;

		/* 端末のread */
		if (0 < readPty(pane->term)) {
			pane->redraw_flag = 1;
			/* IMEスポット移動 */
			if (win->ime.xic) {
				win->ime.spot.x = pane->xpad + pane->term->cx * xfont->cw;
				win->ime.spot.y = pane->ypad + pane->term->cy * xfont->ch + xfont->ascent;
				XSetICValues(win->ime.xic, XNPreeditAttributes, win->ime.spotlist, NULL);
			}
		} else if (errno == EIO) {
			return;
		}

		/* 点滅させる必要がないときはキャレットのタイマーを止める */
		pane->timer_active[CARET_TIMER] = pane->focus &&
			(pane->term->cy + pane->scr <= pane->term->sb->rows) &&
			(!pane->term->ctype || pane->term->ctype % 2);

		/* タイムアウトの設定 */
		nexttime = (struct timespec){ 1 << 16, 0 };
		timespecadd(&now, &nexttime, &nexttime);
		for (i = 0; i < TIMER_NUM; i++) {
			if (!pane->timer_active[i])
				continue;
			if (timespeccmp(&pane->timers[i], &now, <=)) {
				pane->timer_lit[i] = !pane->timer_lit[i];
				pane->redraw_flag = 1;
				timespecadd(&pane->timers[i], &duration[i], &pane->timers[i]);
				if (timespeccmp(&pane->timers[i], &now, <=))
					timespecadd(&now, &duration[i], &pane->timers[i]);
			}
			nexttime = *CHOOSE(CHOOSE(&pane->timers[i], &nexttime, <), &now, >);
		}
		timespecsub(&nexttime, &now, &timeout);

		/* ベルは繰り返さない */
		if (pane->timer_lit[BELL_TIMER] == 0)
			pane->timer_active[BELL_TIMER] = 0;

		/* 再描画 */
		if (pane->redraw_flag)
			redraw(win);
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

Pane *
createPane(Display *disp, Drawable d, int width, int height, int depth)
{
	Pane *pane = xmalloc(sizeof(Pane));

	pane->pixmap = XCreatePixmap(disp, d, width, height, depth);
	pane->gc = XCreateGC(disp, pane->pixmap, 0, NULL);
	pane->draw = XftDrawCreate(disp, pane->pixmap, visual, cmap);
	pane->d = d;
	pane->depth = depth;
	pane->width = width;
	pane->height = height;
	pane->xpad = xfont->cw / 2;
	pane->ypad = xfont->cw / 2;
	memset(&pane->timers, 0, TIMER_NUM * sizeof(struct timespec));
	memset(&pane->timer_active, 0, TIMER_NUM);
	memset(&pane->timer_lit, 0, TIMER_NUM);

	return pane;
}

void
destroyPane(Pane *pane)
{
	XftDrawDestroy(pane->draw);
	XFreeGC(disp, pane->gc);
	XFreePixmap(disp, pane->pixmap);
	free(pane);
}

void
setPaneSize(Pane *pane, int width, int height)
{
	XftDrawDestroy(pane->draw);
	XFreeGC(disp, pane->gc);
	XFreePixmap(disp, pane->pixmap);

	pane->pixmap = XCreatePixmap(disp, pane->d, width, height, pane->depth);
	pane->gc = XCreateGC(disp, pane->pixmap, 0, NULL);
	pane->draw = XftDrawCreate(disp, pane->pixmap, visual, cmap);
	pane->width = width;
	pane->height = height;
}

Win *
openWindow(void)
{
	Pane *pane = createPane(disp, DefaultRootWindow(disp), 800, 600, 32);
	Win *win = xmalloc(sizeof(Win));

	*win = (Win) { .width = 800, .height = 600, .pane = pane };

	/* 端末をオープン */
	pane->term = openTerm( (win->height - pane->ypad * 2) / xfont->ch,
			(win->width - pane->xpad * 2) / xfont->cw, 256);
	if (!pane->term)
		errExit("openTerm failed.\n");
	pane->term->bell = bell;
	pane->term->palette[defbg] = 0xcc000000 + (0x00ffffff & pane->term->palette[defbg]);

	/* ウィンドウの属性 */
	win->attr.event_mask = KeyPressMask | KeyReleaseMask |
		ExposureMask | FocusChangeMask | StructureNotifyMask |
		ButtonPressMask | ButtonReleaseMask | ButtonMotionMask;
	win->attr.colormap = cmap;
	win->attr.border_pixel = pane->term->palette[defbg];

	/* ウィンドウ作成 */
	win->window = XCreateWindow(disp, DefaultRootWindow(disp),
			0, 0, win->width, win->height, 1, 32, InputOutput, visual,
			CWEventMask | CWColormap | CWBorderPixel, &win->attr);

	/* プロパティ */
	win->hint = XAllocClassHint();
	win->hint->res_name = "chitan";
	win->hint->res_class = "chitan";
	XSetClassHint(disp, win->window, win->hint);

	/* 描画の準備 */
	win->gc = XCreateGC(disp, win->window, 0, NULL);
	win->draw = XftDrawCreate(disp, win->window, visual, cmap);
	XSetForeground(disp, win->gc, pane->term->palette[deffg]);
	XSetBackground(disp, win->gc, pane->term->palette[defbg]);

	/* IME */
	xicOpen(win);
	win->ime.spotlist = XVaCreateNestedList(0,
			XNSpotLocation, &win->ime.spot,
			NULL);
	win->ime.peline = allocLine();

	/* ウィンドウが閉じられたときイベントを受け取る */
	Atom atom = XInternAtom(disp, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(disp, win->window, &atom, 1);

	/* ウィンドウを表示 */
	XMapWindow(disp, win->window);
	XSync(disp, False);

	return win;
}

void
closeWindow(Win *win)
{
	free(win->pane->selection.primary);
	free(win->pane->selection.clip);
	destroyPane(win->pane);
	freeLine(win->ime.peline);
	XFree(win->ime.spotlist);
	XFree(win->ime.peattrs);
	if (win->ime.xic)
		XDestroyIC(win->ime.xic);
	XftDrawDestroy(win->draw);
	XFreeGC(disp, win->gc);
	XFree(win->hint);
	XDestroyWindow(disp, win->window);
	closeTerm(win->pane->term);
	free(win);
}

char
handleXEvent(Win *win)
{
	Pane *pane = win->pane;
	XEvent event;
	XConfigureEvent *e;
	XSelectionRequestEvent *sre;
	int mx, my, state, mb = 0;
	Atom prop, type;
	int format;
	unsigned long ntimes, after;
	unsigned char *props;
	char *sel;

	while (0 < XPending(disp)) {
		XNextEvent(disp, &event);

		/* IMEのイベントをフィルタリング */
		if (!xim)
			ximOpen(disp, NULL, NULL);
		if (xim && XFilterEvent(&event, None) == True)
			continue;

		switch (event.type) {
		case KeyPress:
			/* キーボード入力 */
			if (keyPressEvent(win, event, 64)) {
				pane->timer_lit[CARET_TIMER] = 1;
				pane->timers[CARET_TIMER] = now;
				pane->scr = 0;
			}
			break;

		case ButtonPress:
			state = event.xbutton.state;
			mb = event.xbutton.button;
			/* スクロール */
			if ((mb == 4 || mb == 5) && pane->term->sb == &pane->term->ori) {
				pane->redraw_flag = 1;
				pane->scr += (mb == 4 ? 1 : -1) * 3;
				pane->scr = CLIP(pane->scr, 0, SCROLLMAX(pane->term->sb));
				break;
			}
			/* 擬似端末に通知 */
			if (!BETWEEN(mb, 1, 4) || (state & ~(ShiftMask | Mod1Mask)) ||
					(pane->term->sb == &pane->term->alt && !(state & ShiftMask))) {
				mouseEvent(pane, &event);
				break;
			}
			/* 貼り付け */
			if (mb == 2) {
				XConvertSelection(disp, atoms[PRIMARY], atoms[UTF8_STRING],
						atoms[MY_SELECTION], win->window, event.xkey.time);
				pane->scr = 0;
				break;
			}
			/* 範囲選択のドラッグを開始 */
			pane->selection.rect = 0 < (state & Mod1Mask);
			pane->selection.altbuf = pane->term->sb == &pane->term->alt;
			pane->selection.dragging = 1;

		case MotionNotify:
			/* 疑似端末に通知 */
			if (!pane->selection.dragging) {
				mouseEvent(pane, &event);
				break;
			}

			/* ここからPress/Motion共通の処理 */
			/* 範囲選択の終点を設定 */
			mx = (event.xbutton.x - pane->xpad) / xfont->cw;
			my = (event.xbutton.y - pane->ypad) / xfont->ch;
			pane->selection.bcol = mx;
			pane->selection.bline = my + pane->term->sb->firstline - pane->scr;
			/* 範囲選択の始点を設定 */
			if (mb == 1) {
				pane->selection.acol = mx;
				pane->selection.aline = my + pane->term->sb->firstline- pane->scr;
			}
			pane->redraw_flag = 1;
			break;

		case ButtonRelease:
			/* 疑似端末に通知 */
			if (!pane->selection.dragging) {
				mouseEvent(pane, &event);
				break;
			}
			/* 範囲選択のドラッグを終了 */
			pane->selection.dragging = 0;
			if (pane->selection.aline == pane->selection.bline &&
			    pane->selection.acol  == pane->selection.bcol)
				break;
			XSetSelectionOwner(disp, atoms[PRIMARY], win->window, event.xkey.time);
			copySelection(pane, &pane->selection.primary);
			break;

		case Expose:
			/* 再描画 */
			pane->redraw_flag = 1;
			break;

		case ConfigureNotify:
			/* ウィンドウサイズ変更 */
			e = (XConfigureEvent *)&event;
			if (win->width == e->width && win->height == e->height)
				break;
			win->width = e->width;
			win->height = e->height;
			setPaneSize(pane, e->width, e->height);
			setWinSize(pane->term,
					(win->height - pane->ypad * 2) / xfont->ch,
					(win->width - pane->xpad * 2) / xfont->cw,
					e->width, e->height);
			break;

		case FocusIn:
		case FocusOut:
			/* フォーカスの変化 */
			pane->focus = event.type == FocusIn;
			pane->redraw_flag = 1;
			break;

		case ClientMessage:
			/* ウィンドウが閉じられた */
			return -1;

		case SelectionRequest:
			/* 貼り付ける文字列を送る */
			sre = &event.xselectionrequest;
			sel = sre->selection == atoms[PRIMARY] ?
				pane->selection.primary : pane->selection.clip;
			if (!sel)
				sel = "";
			if (sre->property == None)
				sre->property = sre->target;
			XChangeProperty(disp, sre->requestor, sre->property, atoms[UTF8_STRING],
					8, PropModeReplace, (unsigned char *)sel, strlen(sel));
			XSelectionEvent se = { SelectionNotify, 0, True, disp, sre->requestor,
				sre->selection, sre->target, sre->property, sre->time };
			XSendEvent(disp, event.xselectionrequest.requestor, False, 0, (XEvent *)&se);
			break;

		case SelectionNotify:
			/* 貼り付ける文字列が届いた */
			if ((prop = event.xselection.property) != None) {
				XGetWindowProperty(disp, win->window, prop, 0, 256,
						False, atoms[UTF8_STRING], &type,
						&format, &ntimes, &after, &props);
				if (1 < pane->term->dec[2004])
					writePty(pane->term, "\e[200~", 6);
				writePty(pane->term, (char *)props, ntimes);
				if (1 < pane->term->dec[2004])
					writePty(pane->term, "\e[201~", 6);
				XFree(props);
			}
		}
	}

	return 0;
}

void
mouseEvent(Pane *pane, XEvent *event)
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
	reportMouse(pane->term, mb, event->type == ButtonRelease,
			(event->xbutton.x - pane->xpad) / xfont->cw,
			(event->xbutton.y - pane->ypad) / xfont->ch);
}

void
copySelection(Pane *pane, char **dst)
{
	int len = 256;
	char32_t *cp, *copy = xmalloc(len * sizeof(copy[0]));
	int firstline = MIN(pane->selection.aline, pane->selection.bline);
	int lastline  = MAX(pane->selection.aline, pane->selection.bline);
	int left      = MIN(pane->selection.acol,  pane->selection.bcol);
	int right     = MAX(pane->selection.acol,  pane->selection.bcol);
	Line *line;
	int i, l, r;

	copy[0] = L'\0';

	/* 選択範囲の文字列(UTF32)を読み出してコピー */
	for(i = firstline; i <= lastline; i++) {
		if (!(line = getLine(pane->term, i - pane->term->sb->firstline)))
			continue;

		if (pane->selection.rect) {
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
	*dst = xrealloc(*dst, len * 4);
	wcstombs(*dst, (wchar_t *)copy, len * 4);

	free(copy);
}

char
keyPressEvent(Win *win, XEvent event, int bufsize)
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

	/* 入力文字列を取得 */
	len = win->ime.xic ?
		Xutf8LookupString(win->ime.xic, &event.xkey, buf, bufsize, &keysym, &status) :
		XLookupString(&event.xkey, buf, bufsize, &keysym, NULL);
	if (status == XBufferOverflow)
		return keyPressEvent(win, event, len);

	/* IMEの確定っぽい場合(コールバックがすぐ来ない場合があるため) */
	if (1 < strlen(buf))
		PUT_NUL(win->ime.peline, 0);

	/* C-S-cでコピー */
	if (keysym == XK_C && event.xkey.state & ControlMask) {
		copySelection(win->pane, &win->pane->selection.clip);
		XSetSelectionOwner(disp, atoms[CLIPBOARD], win->window, event.xkey.time);
		return 0;
	}

	/* C-S-vで貼り付け */
	if (keysym == XK_V && event.xkey.state & ControlMask) {
		XConvertSelection(disp, atoms[CLIPBOARD], atoms[UTF8_STRING], atoms[MY_SELECTION],
				win->window, event.xkey.time);
		return 1;
	}

	if (strlen(buf)) {
		/* 文字列を送る */
		if (event.xkey.state & Mod1Mask)
			writePty(win->pane->term, "\e", 1);
		writePty(win->pane->term, buf, len);
		return 1;
	} else {
		/* カーソルキー等を送る */
		for (key = keys; key->symbol != XK_VoidSymbol; key++) {
			if (key->symbol != keysym)
				continue;
			str = win->pane->term->dec[1] < 2 ? key->normal : key->app;
			writePty(win->pane->term, str, strlen(str));
			return 1;
		}
	}

	return 0;
}

void
redraw(Win *win)
{
	drawPane(win->pane, &win->ime);
	XCopyArea(disp, win->pane->pixmap, win->window, win->gc, 0, 0,
			win->pane->width, win->pane->height, 0, 0);
	XSync(disp, False);
}

void
drawPane(Pane *pane, const IME *ime)
{
	Line *line;
	int pepos, pewidth, pecaretpos, caretrow;
	int i;

	/* スクロール量の更新 */
	pane->scr = (pane->prevbuf != pane->term->sb) ? 0 : pane->scr;
	pane->scr += (0 < pane->scr) ? pane->term->sb->firstline - pane->prevfst : 0;
	pane->scr = CLIP(pane->scr, 0, SCROLLMAX(pane->term->sb));
	pane->prevbuf = pane->term->sb;
	pane->prevfst = pane->term->sb->firstline;

	/* 点滅中フラグをクリア */
	pane->timer_active[BLINK_TIMER] = pane->timer_active[RAPID_TIMER] = 0;

	/* 画面をクリア */
	XSetForeground(disp, pane->gc, BELLCOLOR(pane->term->palette[defbg]));
	XFillRectangle(disp, pane->pixmap, pane->gc, 0, 0, pane->width, pane->height);

	/* 端末の内容をウィンドウに表示 */
	for (i = 0; i <= pane->term->sb->rows; i++)
		if ((line = getLine(pane->term, i - pane->scr)))
			drawLine(pane, line, i, 0, pane->term->sb->cols, 0);

	/* カーソルかPreeditを表示 */
	XSetForeground(disp, pane->gc, pane->term->palette[deffg]);
	if (u32slen(ime->peline->str)) {
		/* Preeditの幅とキャレットのPreedit内での位置を取得 */
		pewidth = u32swidth(ime->peline->str, u32slen(ime->peline->str));
		pecaretpos = u32swidth(ime->peline->str, ime->caret);

		/* Preeditの画面上での描画位置を決める */
		pepos = pane->term->cx - pecaretpos;
		pepos = MIN(pepos, 0);
		pepos = MAX(pepos, pane->term->sb->cols - pewidth);
		pepos = MIN(pepos, pane->term->cx);

		/* Preeditとカーソルの描画 */
		drawLine(pane, ime->peline, pane->term->cy, pepos, pewidth, 0);
		drawCursor(pane, ime->peline, pane->term->cy, pepos + pecaretpos, 6);
	} else if (1 <= pane->term->dec[25] && pane->term->cx < pane->term->sb->cols) {
		/* カーソルの描画 */
		caretrow = pane->term->cy + pane->scr;
		if (caretrow <= pane->term->sb->rows && (line = getLine(pane->term, pane->term->cy)))
			drawCursor(pane, line, caretrow, pane->term->cx, pane->term->ctype);
	}

	/* 選択範囲を書く */
	if ((pane->selection.aline != pane->selection.bline ||
	     pane->selection.acol  != pane->selection.bcol) &&
	    pane->selection.altbuf == (pane->term->sb == &pane->term->alt))
		drawSelection(pane, &pane->selection);

	pane->redraw_flag = 0;
}

void
drawLine(Pane *pane, Line *line, int row, int col, int width, int pos)
{
	int next, i = getCharCnt(line->str, pos).index;
	int x, y, w;
	int attr, fg, bg;
	XftColor xc;
	Color c;

	if (width <= pos || line->str[i] == L'\0')
		return;

	/* 同じ属性の文字はまとめて処理する */
	next = MIN(findNextSGR(line, i), width);
	drawLine(pane, line, row, col, width, pos + u32swidth(&line->str[i], next - i));

	/* 点滅 */
	if (line->attr[i] & BLINK)
		pane->timer_active[BLINK_TIMER] = 1;
	if (line->attr[i] & RAPID)
		pane->timer_active[RAPID_TIMER] = 1;
	if (!(((line->attr[i] & BLINK) && pane->timer_lit[BLINK_TIMER]) ||
	      ((line->attr[i] & RAPID) && pane->timer_lit[RAPID_TIMER]) ||
	      (!(line->attr[i] & BLINK) && !(line->attr[i] & RAPID))))
		return;

	/* 座標 */
	x = pane->xpad + (col + pos) * xfont->cw;
	y = pane->ypad + row * xfont->ch;
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
	c = pane->term->palette[fg]; /* 色を取得 */
	if (line->attr[i] & FAINT) /* 細字 */
		c = BLEND_COLOR(c, 0.6, pane->term->palette[bg], 0.4);

	/* 色をXftColorに変換 */
	xc.color.red   =   RED(c) << 8;
	xc.color.green = GREEN(c) << 8;
	xc.color.blue  =  BLUE(c) << 8;
	xc.color.alpha = 0xffff;

	/* 背景を塗る */
	XSetForeground(disp, pane->gc, BELLCOLOR(pane->term->palette[bg]));
	XFillRectangle(disp, pane->pixmap, pane->gc, x, y, w, xfont->ch);

	y += xfont->ascent;

	/* 文字を書く */
	attr = FONT_NONE;
	attr |= line->attr[i] & BOLD   ? FONT_BOLD   : FONT_NONE;
	attr |= line->attr[i] & ITALIC ? FONT_ITALIC : FONT_NONE;
	drawXFontString(pane->draw, &xc, xfont, attr, x, y, &line->str[i], next - i);

	/* 後処理 */
	if (line->attr[i] & ULINE) { /* 下線 */
		XSetForeground(disp, pane->gc, pane->term->palette[fg]);
		XDrawLine(disp, pane->pixmap, pane->gc, x, y + 1, x + w - 1, y + 1);
	}
}

void
drawCursor(Pane *pane, Line *line, int row, int col, int type)
{
	const int index = getCharCnt(line->str, col).index;
	char32_t *c = index < u32slen(line->str) ? &line->str[index] : (char32_t *)L" ";
	const int x = pane->xpad + col * xfont->cw;
	const int y = pane->ypad + row * xfont->ch;
	const int width = xfont->cw * u32swidth(c, 1) - 1;
	int attr;
	Line cursor;

	/* 点滅 */
	if (pane->focus && (!type || type % 2) && (pane->timer_lit[CARET_TIMER]))
		return;

	switch (type) {
	default: /* ブロック */
	case 0:
	case 1:
	case 2:
		if (pane->focus) {
			attr = index < u32slen(line->str) ? line->attr[index] : 0;
			cursor = (Line){c, &attr, &defbg, &deffg};
			drawLine(pane, &cursor, row, col, 1, 0);
		} else {
			XSetForeground(disp, pane->gc, BELLCOLOR(pane->term->palette[deffg]));
			XDrawRectangle(disp, pane->pixmap, pane->gc, x, y, width, xfont->ch - 1);
			XDrawPoint(disp, pane->pixmap, pane->gc, x + width, y + xfont->ch - 1);
		}
		break;
	case 3: /* 下線 */
	case 4:
		XFillRectangle(disp, pane->pixmap, pane->gc, x, y + 1 + xfont->ascent, width, xfont->ch * 0.1);
		break;
	case 5: /* 縦線 */
	case 6:
		XFillRectangle(disp, pane->pixmap, pane->gc, x - 1, y, xfont->ch * 0.1, xfont->ch);
		break;
	}
}

void
drawSelection(Pane *pane, struct Selection *sel)
{
	const int s = MIN(sel->aline, sel->bline) - pane->term->sb->firstline;
	const int e = MAX(sel->aline, sel->bline) - pane->term->sb->firstline;
	int i;

#define DRAW(n, a, b)   drawLineRev(pane, getLine(pane->term, n), n + pane->scr, a, b)
	if (sel->rect) {
		/* 矩形選択 */
		for (i = s; i < e + 1; i++)
			DRAW(i, sel->acol, sel->bcol);
	} else {
		/* 通常 */
		if (sel->aline == sel->bline) {
			DRAW(s, sel->acol, sel->bcol);
		} else {
			DRAW(s, (sel->aline < sel->bline ? sel->acol : sel->bcol), pane->term->sb->cols + 1);
			DRAW(e, 0, (sel->aline < sel->bline ? sel->bcol : sel->acol));
			for (i = s + 1; i < e; i++)
				DRAW(i, 0, pane->term->sb->cols + 1);
		}
	}
#undef DRAW
}

void
drawLineRev(Pane *pane, Line *line, int row, int col1, int col2)
{
	const Color c = pane->term->palette[defbg];
	XftColor xc = { 0, { RED(c) << 8, GREEN(c) << 8, BLUE(c) << 8, 0xffff } };
	int li, ri;
	int xoff, yoff, len;

	if (!BETWEEN(row, 0, pane->term->sb->rows + 1) || (!line))
		return;

	li = MIN(getCharCnt(line->str, MIN(col1, col2)).index, u32slen(line->str));
	ri = MIN(getCharCnt(line->str, MAX(col1, col2)).index, u32slen(line->str));

	xoff = pane->xpad + getCharCnt(line->str, MIN(col1, col2)).col * xfont->cw;
	yoff = pane->ypad + row * xfont->ch;
	len = MIN(u32slen(line->str + li), ri - li);

	XSetForeground(disp, pane->gc, BELLCOLOR(pane->term->palette[deffg]));
	XFillRectangle(disp, pane->pixmap, pane->gc, xoff, yoff,
			u32swidth(line->str + li, len) * xfont->cw, xfont->ch);
	drawXFontString(pane->draw, &xc, xfont, 0, xoff, yoff + xfont->ascent,
			line->str + li, len);
}

void
bell(void *term)
{
	win->pane->timer_active[BELL_TIMER] = 1;
	win->pane->timers[BELL_TIMER] = now;
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
