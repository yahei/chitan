#include <sys/select.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <X11/Xlib.h>

#include "pane.h"
#include "util.h"

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

typedef struct Win {
	Window window;
	XClassHint *hint;
	XSetWindowAttributes attr;
	IME ime;
	GC gc;
	XftDraw *draw;
	int width, height;
	Pane *pane;
	char name[TITLE_MAX];
	char *primary, *clip;
} Win;

enum { PRIMARY, CLIPBOARD, UTF8_STRING, MY_SELECTION, WM_DELETE_WINDOW, ATOM_NUM };

static Atom atoms[ATOM_NUM];
static DispInfo dinfo;
static XFont *xfont;
static XIM xim;
static Win *win;
static struct timespec now;
static float alpha;
static int loglines;
static char **cmd, *pattern;

static void init(void);
static void run(void);
static void fin(void);

/* Win */
static Win *openWindow(int ,int);
static void closeWindow(Win *);
static void setWindowName(Win *, char *);
static int handleXEvent(Win *);
static int keyPressEvent(Win *, XEvent, int);
static void redraw(Win *);

/* IME */
static void ximOpen(Display *, XPointer, XPointer);
static void imDestroy(XIM, XPointer, XPointer);
static void xicOpen(Win *);
static int icDestroy(XIC, Win *, XPointer);
static void preeditStart(XIM, Win *, XPointer);
static void preeditDone(XIM, Win *, XPointer);
static void preeditDraw(XIM, Win *, XIMPreeditDrawCallbackStruct *);
static void preeditCaret(XIM, Win *, XIMPreeditCaretCallbackStruct *);

static char *version = "chitan 0.0.0";
static char *help = "usage: chitan [-options] [[-e] command [args ...]]\n"
"        -a alpha                background opacity (0.0-1.0)\n"
"        -f font                 font selection pattern (ex. monospace:size=12)\n"
"        -h                      show this help\n"
"        -l number               number of log lines\n"
"        -v                      show version\n"
"        -e command [args ...]   command to execute (must be the last)\n";

int
main(int argc, char *argv[])
{
	alpha = 1.0;
	loglines = 1024;
	pattern = "monospace";

	while (1) {
		switch (getopt(argc, argv, "+a:f:l:hve:")) {
		case '?': printf("%s", help);                   return 0;
		case 'a': alpha = CLIP(atof(optarg), 0, 1.0);   continue;
		case 'f': pattern = optarg;                     continue;
		case 'h': printf("%s", help);                   return 0;
		case 'l': loglines = MAX(atoi(optarg), 1);      continue;
		case 'v': printf("%s\n", version);              return 0;
		case 'e': cmd = argv + optind - 1;              break;
		default : cmd = argv + optind;                  break;
		}
		break;
	}

	cmd    = cmd[0] ? cmd    : (char *[]){ getenv("SHELL"), NULL };
	cmd[0] = cmd[0] ? cmd[0] : "/bin/sh";

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
	dinfo.disp= XOpenDisplay(NULL);
	if (dinfo.disp== NULL)
		fatal("XOpenDisplay failed.\n");
	XMatchVisualInfo(dinfo.disp, XDefaultScreen(dinfo.disp), 32, TrueColor, &vinfo);
	dinfo.visual = vinfo.visual;
	dinfo.cmap = XCreateColormap(dinfo.disp, DefaultRootWindow(dinfo.disp), dinfo.visual, None);

	/* XIM */
	XRegisterIMInstantiateCallback(dinfo.disp, NULL, NULL, NULL,
			ximOpen, NULL);

	/* 文字描画の準備 */
	FcInit();
	xfont = openFont(dinfo.disp, pattern);
	if (xfont == NULL)
		fatal("XftFontOpen failed.\n");

	/* Selection */
	names = (char *[]){ "PRIMARY", "CLIPBOARD", "UTF8_STRING", "_MY_SELECTION_", "WM_DELETE_WINDOW" };
	for (i = 0; i < ATOM_NUM; i++)
		atoms[i] = XInternAtom(dinfo.disp, names[i], True);
	
	/* ウィンドウの作成 */
	win = openWindow(800, 600);
}

void
run(void)
{
	Pane *pane = win->pane;
	struct timespec timeout = { 0, 1000 }, time;
	fd_set rfds;
	int tfd = pane->term->master;
	int xfd = XConnectionNumber(dinfo.disp);

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
		if (handleXEvent(win))
			return;

		/* 端末のread */
		errno = 0;
		while (0 < readPty(pane->term)) {
			pane->redraw_flag = true;
			clock_gettime(CLOCK_MONOTONIC, &time);
			if (20 * 1000 * 1000 < tstons(time) - tstons(now))
				break;
			usleep(1);
		}
		if (errno == EIO)
			return;

		/* IMEスポット移動 */
		if (pane->redraw_flag && win->ime.xic) {
			win->ime.spot.x = pane->xpad + pane->term->cx * xfont->cw;
			win->ime.spot.y = pane->ypad + pane->term->cy * xfont->ch + xfont->ascent;
			XSetICValues(win->ime.xic, XNPreeditAttributes, win->ime.spotlist, NULL);
		}

		/* 再描画 */
		if (pane->redraw_flag || (!FD_ISSET(tfd, &rfds) && !FD_ISSET(xfd, &rfds)))
			redraw(win);

		/* 次の待機時間を取得 */
		getNextTime(pane, &timeout, tstons(now));
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
	XCloseDisplay(dinfo.disp);
}

Win *
openWindow(int width, int height)
{
	Win *win = xmalloc(sizeof(Win));

	*win = (Win){ .width = width, .height = height };

	/* ウィンドウの属性 */
	win->attr.event_mask = KeyPressMask | KeyReleaseMask |
		ExposureMask | FocusChangeMask | StructureNotifyMask |
		ButtonPressMask | ButtonReleaseMask | ButtonMotionMask;
	win->attr.colormap = dinfo.cmap;

	/* ウィンドウ作成 */
	win->window = XCreateWindow(dinfo.disp, DefaultRootWindow(dinfo.disp),
			0, 0, width, height, 1, 32, InputOutput, dinfo.visual,
			CWEventMask | CWColormap | CWBorderPixel, &win->attr);

	/* プロパティ */
	win->hint = XAllocClassHint();
	*win->hint = (XClassHint){ "chitan-256color", "chitan-256color" };
	XSetClassHint(dinfo.disp, win->window, win->hint);

	/* 描画の準備 */
	win->gc = XCreateGC(dinfo.disp, win->window, 0, NULL);
	win->draw = XftDrawCreate(dinfo.disp, win->window, dinfo.visual, dinfo.cmap);

	/* IME */
	xicOpen(win);
	win->ime.spotlist = XVaCreateNestedList(0,
			XNSpotLocation, &win->ime.spot,
			NULL);
	win->ime.peline = allocLine();

	/* ウィンドウが閉じられたときイベントを受け取る */
	XSetWMProtocols(dinfo.disp, win->window, &atoms[WM_DELETE_WINDOW], 1);

	/* ウィンドウを表示 */
	XMapWindow(dinfo.disp, win->window);
	XFlush(dinfo.disp);

	/* Pane作成 */
	win->pane = createPane(&dinfo, xfont, width, height, alpha, loglines, cmd);

	return win;
}

void
closeWindow(Win *win)
{
	destroyPane(win->pane);
	freeLine(win->ime.peline);
	XFree(win->ime.spotlist);
	XFree(win->ime.peattrs);
	if (win->ime.xic)
		XDestroyIC(win->ime.xic);
	XftDrawDestroy(win->draw);
	XFreeGC(dinfo.disp, win->gc);
	XFree(win->hint);
	XDestroyWindow(dinfo.disp, win->window);
	free(win->primary);
	free(win->clip);
	free(win);
}

void
setWindowName(Win *win, char *name)
{
	XTextProperty prop = {
		(unsigned char *)name, atoms[UTF8_STRING], 8, strlen(name)
	};

	if (strcmp(win->name, name) != 0) {
		strncpy(win->name, name, TITLE_MAX);
		XSetWMName(dinfo.disp, win->window, &prop);
	}
}

int
handleXEvent(Win *win)
{
	static Pane *dragging = NULL;
	Pane *pane = win->pane;
	XEvent event;
	const XConfigureEvent *ce = (XConfigureEvent *)&event;
	const XClientMessageEvent *cme = (XClientMessageEvent *)&event;
	XSelectionRequestEvent *sre;
	XSelectionEvent se;
	int mx, my, state, mb = 0;
	Atom prop, type;
	int format;
	unsigned long ntimes, after;
	unsigned char *props;
	char *sel;

	while (0 < XPending(dinfo.disp)) {
		XNextEvent(dinfo.disp, &event);

		/* IMEのイベントをフィルタリング */
		if (!xim)
			ximOpen(dinfo.disp, NULL, NULL);
		if (xim && XFilterEvent(&event, None) == True)
			continue;

		mx = (event.xbutton.x - pane->xpad + xfont->cw / 2) / xfont->cw;
		my = (event.xbutton.y - pane->ypad) / xfont->ch;

		switch (event.type) {
		case KeyPress:          /* キーボード入力 */
			if (keyPressEvent(win, event, 64)) {
				pane->caret_time = tstons(now);
				pane->scr = 0;
			}
			break;

		case ButtonPress:       /* マウス Press */
			state = event.xbutton.state;
			mb = event.xbutton.button;
			if ((mb == 4 || mb == 5) && pane->term->sb == &pane->term->ori) {
				scrollPane(pane, (mb == 4 ? 1 : -1) * 3);
			} else if (!BETWEEN(mb, 1, 4) || (state & ~(ShiftMask | Mod1Mask)) ||
					(pane->term->sb == &pane->term->alt && !(state & ShiftMask))) {
				mouseEvent(pane, &event);
			} else if (mb == 2) {
				XConvertSelection(dinfo.disp, atoms[PRIMARY], atoms[UTF8_STRING],
						atoms[MY_SELECTION], win->window, event.xkey.time);
				pane->scr = 0;
			} else {
				dragging = pane;
				selectPane(pane, my, mx, mb == 1, 0 < (state & Mod1Mask));
			}
			break;

		case MotionNotify:     /* マウス Move */
			if (!dragging)
				mouseEvent(pane, &event);
			else
				selectPane(dragging, my, mx, false, pane->sel.rect);
			break;

		case ButtonRelease:    /* マウス Release */
			if (!dragging) {
				mouseEvent(pane, &event);
			} else {
				if (dragging->sel.aline == dragging->sel.bline &&
				    dragging->sel.acol  == dragging->sel.bcol)
					break;
				XSetSelectionOwner(dinfo.disp, atoms[PRIMARY], win->window, event.xkey.time);
				copySelection(&dragging->sel, &win->primary, !dragging->sel.rect);
				dragging = NULL;
			}
			break;

		case Expose:            /* 再描画 */
			pane->redraw_flag = true;
			break;

		case ConfigureNotify:   /* ウィンドウサイズ変更 */
			if (win->width == ce->width && win->height == ce->height)
				break;
			win->width  = ce->width;
			win->height = ce->height;
			setPaneSize(pane, ce->width, ce->height);
			break;

		case FocusIn:
		case FocusOut:          /* フォーカスの変化 */
			pane->focus = event.type == FocusIn;
			pane->redraw_flag = true;
			break;

		case ClientMessage:     /* ウィンドウが閉じられた */
			return cme->data.l[0] == atoms[WM_DELETE_WINDOW];

		case SelectionRequest:  /* 貼り付ける文字列を送る */
			sre = &event.xselectionrequest;
			sel = sre->selection == atoms[PRIMARY] ? win->primary : win->clip;
			if (!sel)
				sel = "";
			if (sre->property == None)
				sre->property = sre->target;
			XChangeProperty(dinfo.disp, sre->requestor, sre->property, atoms[UTF8_STRING],
					8, PropModeReplace, (unsigned char *)sel, strlen(sel));
			se = (XSelectionEvent){ SelectionNotify, 0, True, dinfo.disp,
				sre->requestor, sre->selection, sre->target, sre->property, sre->time };
			XSendEvent(dinfo.disp, event.xselectionrequest.requestor, False, 0, (XEvent *)&se);
			break;

		case SelectionNotify:   /* 貼り付ける文字列が届いた */
			if ((prop = event.xselection.property) != None) {
				XGetWindowProperty(dinfo.disp, win->window, prop, 0, 256,
						False, atoms[UTF8_STRING], &type,
						&format, &ntimes, &after, &props);
				if (1 < pane->term->dec[2004])
					writePty(pane->term, "\e[200~", 6);
				writePty(pane->term, (char *)props, ntimes);
				if (1 < pane->term->dec[2004])
					writePty(pane->term, "\e[201~", 6);
				XFree(props);
			}
			break;
		}
	}

	return 0;
}

int
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
		{ XK_Delete,    "\e[3~",        "\e[3~" },
		{ XK_BackSpace, "\x7f",         "\x7f" },
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
		copySelection(&win->pane->sel, &win->clip, !win->pane->sel.rect);
		XSetSelectionOwner(dinfo.disp, atoms[CLIPBOARD], win->window, event.xkey.time);
		return 0;
	}

	/* C-S-vで貼り付け */
	if (keysym == XK_V && event.xkey.state & ControlMask) {
		XConvertSelection(dinfo.disp, atoms[CLIPBOARD], atoms[UTF8_STRING],
				atoms[MY_SELECTION], win->window, event.xkey.time);
		return 1;
	}

	/* カーソルキー等を送る */
	for (key = keys; key->symbol != XK_VoidSymbol; key++) {
		if (key->symbol != keysym)
			continue;
		str = win->pane->term->dec[1] < 2 ? key->normal : key->app;
		writePty(win->pane->term, str, strlen(str));
		return 1;
	}

	/* 文字列を送る */
	if (strlen(buf)) {
		if (event.xkey.state & Mod1Mask)
			writePty(win->pane->term, "\e", 1);
		writePty(win->pane->term, buf, len);
		return 1;
	}

	return 0;
}

void
redraw(Win *win)
{
	setWindowName(win, win->pane->term->title);
	drawPane(win->pane, tstons(now), win->ime.peline, win->ime.caret);
	XCopyArea(dinfo.disp, win->pane->pixmap, win->window, win->gc, 0, 0,
			win->pane->width, win->pane->height, 0, 0);
	XFlush(dinfo.disp);
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
	len  = call->text->length;
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
