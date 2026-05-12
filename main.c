#include <sys/select.h>
#include <sys/wait.h>
#include <errno.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>

#include "pane.h"
#include "util.h"

typedef struct IME {
	XIC xic;
	XICCallback icdestroy;
	XIMCallback pestart, pedone, pedraw, pecaret;
	XVaNestedList peattrs, spotlist;
	XPoint spot;
	Line *peline;
	int caret;
} IME;

typedef struct Win {
	Window window;
	XClassHint *hint;
	XSetWindowAttributes attr;
	GC gc;
	XftDraw *draw;
	int width, height;
	char name[TITLE_MAX];
	char *primary, *clip;
	IME ime;
	Pane *pane, *dragging;
} Win;

typedef struct TTArgs {
	Term *term;
	int r_pipe, e_pipe;
	pthread_mutex_t *mtx;
} TTArgs;

enum { CLIPBOARD, UTF8_STRING, WM_DELETE_WINDOW, ATOM_NUM };

static Atom atoms[ATOM_NUM];
static DispInfo dinfo;
static XFont *xfont;
static XIM xim;
static Win *win;
static struct timespec now;
static int redraw_pipe[2];
static int exit_pipe[2];
static int sigchld_pipe[2];

static void init(int, char *[]);
static void initPalette(Term *, float);
static void run(void);
static void *termThread(TTArgs *);
static void fin(void);
static void sigHandler(int);

/* Win */
static Win *openWindow(int ,int, int, int, int, float, char *const []);
static void closeWindow(Win *);
static void setWindowName(Win *, const char *);
static int handleXEvent(Win *);
static int keyPressEvent(Win *, XEvent, int);
static void sendSelection(Win *, XEvent);
static void receiveSelection(Win *, Pane *, XEvent);
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

static const char version[] = "chitan 0.2.0";
static const char help[] = "usage: chitan [-options] [[-e] command [args ...]]\n"
"        -a alpha                background opacity (0.0-1.0)\n"
"        -f font                 font selection pattern (ex. monospace:size=12)\n"
"        -g geometry             size (in chars) and position (ex. 80x24+0+0)\n"
"        -h                      show this help\n"
"        -l number               number of lines in buffer\n"
"        -v                      show version\n"
"        -e command [args ...]   command to execute (must be the last)\n";

int
main(int argc, char *argv[])
{
	init(argc, argv);
	run();
	fin();
	return 0;
}

void
init(int argc, char *argv[])
{
	const struct sigaction act = {
		.sa_handler = sigHandler,
		.sa_flags = SA_NOCLDSTOP,
	};
	XVisualInfo vinfo;
	char *xrm, *str_type;
	XrmDatabase xdb;
	XrmValue val;
	float alpha = 1.0;
	int buflines = 1024;
	char pattern_str[256] = "monospace", *pattern = pattern_str;
	char geometry_str[256] = "80x24+0+0", *geometry = geometry_str;
	char **cmd = (char *[]){ NULL };
	char **names;
	unsigned int row, col;
	int x, y, w, h, i;

	/* パイプ作成 */
	if (pipe(redraw_pipe) || pipe(sigchld_pipe) || pipe(exit_pipe))
		fatal("pipe failed.\n");

	/* シグナル受信設定 */
	if (sigaction(SIGCHLD, &act, NULL))
		fatal("sigaction faild.\n");

	/* localeを設定 */
	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers("");

	/* Xサーバーに接続 */
	dinfo.disp= XOpenDisplay(NULL);
	if (dinfo.disp == NULL)
		fatal("XOpenDisplay failed.\n");
	dinfo.screen = XDefaultScreen(dinfo.disp);
	dinfo.root = DefaultRootWindow(dinfo.disp);
	XMatchVisualInfo(dinfo.disp, dinfo.screen, 32, TrueColor, &vinfo);
	dinfo.visual = vinfo.visual;
	dinfo.cmap = XCreateColormap(dinfo.disp, dinfo.root, dinfo.visual, None);

	/* X resources */
	XrmInitialize();
	xrm = XResourceManagerString(dinfo.disp);
	xdb = XrmGetStringDatabase(xrm ? xrm : "");
#define XRES(name) (XrmGetResource(xdb, (name), "chitan", &str_type, &val) &&\
		strncmp(str_type, "String", 6) == 0)
	if (XRES("chitan.alpha"))       alpha    = atof(val.addr);
	if (XRES("chitan.font"))        strcpy(pattern_str, val.addr);
	if (XRES("chitan.geometry"))    strcpy(geometry_str, val.addr);
	if (XRES("chitan.lines"))       buflines = atof(val.addr);
#undef XRES
	XrmDestroyDatabase(xdb);

	/* コマンドライン引数 */
	while (1) {
		switch (getopt(argc, argv, "+a:f:g:l:hve:")) {
		case '?': printf("%s", help);                   goto finish;
		case 'a': alpha = CLIP(atof(optarg), 0, 1.0);   continue;
		case 'f': pattern = optarg;                     continue;
		case 'g': geometry = optarg;                    continue;
		case 'h': printf("%s", help);                   goto finish;
		case 'l': buflines = MAX(atoi(optarg), 1);      continue;
		case 'v': printf("%s\n", version);              goto finish;
		case 'e': cmd = argv + optind - 1;              break;
		default : cmd = argv + optind;                  break;
		}
		break;
finish:
		XCloseDisplay(dinfo.disp);
		exit(0);
	}

	/* Atomを取得 */
	names = (char *[]){ "CLIPBOARD", "UTF8_STRING", "WM_DELETE_WINDOW" };
	for (i = 0; i < ATOM_NUM; i++)
		atoms[i] = XInternAtom(dinfo.disp, names[i], True);
	
	/* XIM */
	XRegisterIMInstantiateCallback(dinfo.disp, NULL, NULL, NULL, ximOpen, NULL);

	/* フォントを用意 */
	FcInit();
	xfont = openFont(dinfo.disp, pattern);
	if (xfont == NULL)
		fatal("XftFontOpen failed.\n");

	/* ウィンドウの作成 */
	x = y = col = row = 0;
	XParseGeometry(geometry, &x, &y, &col, &row);
	cmd    = cmd[0] ? cmd    : (char *[]){ getenv("SHELL"), NULL };
	cmd[0] = cmd[0] ? cmd[0] : "/bin/sh";
	w = col * xfont->cw + xfont->cw;
	h = row * xfont->ch + xfont->cw;
	win = openWindow(w, h, x, y, buflines, alpha, cmd);
}

void
run(void)
{
	Pane *pane = win->pane;
	pthread_t thd_term;
	pthread_mutex_t term_mtx = PTHREAD_MUTEX_INITIALIZER;
	TTArgs ttargs = { pane->term, redraw_pipe[1], exit_pipe[0], &term_mtx};
	struct timespec timeout = { 0, 0 };
	fd_set rfds;
	const int xfd = XConnectionNumber(dinfo.disp);
	const int rfd = redraw_pipe[0];
	const int sfd = sigchld_pipe[0];
	const int nfds = MAX(MAX(xfd, rfd), sfd) + 1;
	char rfd_buf[1024];

	/* 擬似端末を管理するスレッドを作成 */
	pthread_create(&thd_term, NULL, (void *(*)(void*))termThread, &ttargs);

	while (1) {
		/* ファイルディスクリプタの監視 */
		FD_ZERO(&rfds);
		FD_SET(xfd, &rfds);
		FD_SET(rfd, &rfds);
		FD_SET(sfd, &rfds);
		if (pselect(nfds, &rfds, NULL, NULL, &timeout, NULL) < 0) {
			if (errno == EINTR)
				continue;
			else
				errExit("pselect failed.\n");
		}
		clock_gettime(CLOCK_MONOTONIC, &now);

		/* ウィンドウのイベント処理 */
		if (FD_ISSET(xfd, &rfds)) {
			if (handleXEvent(win))
				return;
			continue;
		}

		/* プロセスの終了 */
		if (FD_ISSET(sfd, &rfds))
			break;

		/* 再描画通知 */
		if (FD_ISSET(rfd, &rfds))
			read(redraw_pipe[0], rfd_buf, 1024);

		/* IMEスポット移動 */
		if (win->ime.xic) {
			win->ime.spot.x = pane->d.xpad + pane->term->cx * xfont->cw;
			win->ime.spot.y = pane->d.ypad + pane->term->cy * xfont->ch + xfont->ascent;
			XSetICValues(win->ime.xic, XNPreeditAttributes, win->ime.spotlist, NULL);
		}

		/* 再描画 */
		pthread_mutex_lock(&term_mtx);
		snapshot(win->pane, tstons(now));
		pthread_mutex_unlock(&term_mtx);
		redraw(win);

		/* 次の待機時間を取得 */
		timeout = nstots(getNextTime(&pane->d, tstons(now)));
	}

	write(exit_pipe[1], "e", 1);
	pthread_join(thd_term, NULL);
}

void *
termThread(TTArgs *ttargs)
{
	fd_set rfds;
	const int tfd = ttargs->term->master;
	const int efd = ttargs->e_pipe;
	const int nfds = MAX(tfd, efd) + 1;
	
	while (1) {
		FD_ZERO(&rfds);
		FD_SET(tfd, &rfds);
		FD_SET(efd, &rfds);
		if (pselect(nfds, &rfds, NULL, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;
			else
				errExit("pselect failed.\n");
		}

		/* 疑似端末を読む */
		if (FD_ISSET(tfd, &rfds)) {
			pthread_mutex_lock(ttargs->mtx);
			write(ttargs->r_pipe, "a", 1);
			errno = 0;
			if (readPty(ttargs->term) < 0 && errno != EIO)
				errExit("pty read error.");
			pthread_mutex_unlock(ttargs->mtx);
		}

		/* 終了 */
		if (FD_ISSET(efd, &rfds))
			break;
	}

	return NULL;
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

void
sigHandler(int sig)
{
	waitpid(-1, NULL, WNOHANG);
	write(sigchld_pipe[1], "q", 1);
}

Win *
openWindow(int w, int h, int x, int y, int buflines, float alpha, char *const cmd[])
{
	Win *win = xmalloc(sizeof(Win));
	Term *term;
	const int pad = xfont->cw / 2;
	const int row = (h - pad * 2) / xfont->ch;
	const int col = (w - pad * 2) / xfont->cw;

	*win = (Win){ .width = w, .height = h};

	/* ウィンドウの属性 */
	win->attr.event_mask = KeyPressMask | KeyReleaseMask |
		ExposureMask | FocusChangeMask | StructureNotifyMask |
		ButtonPressMask | ButtonReleaseMask | ButtonMotionMask;
	win->attr.colormap = dinfo.cmap;

	/* ウィンドウ作成 */
	win->window = XCreateWindow(dinfo.disp, DefaultRootWindow(dinfo.disp),
			x, y, w, h, 1, 32, InputOutput, dinfo.visual,
			CWEventMask | CWColormap | CWBorderPixel, &win->attr);

	/* プロパティ */
	win->hint = XAllocClassHint();
	*win->hint = (XClassHint){ "chitan", "chitan" };
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

	/* Term作成 */
	term = openTerm(row, col, buflines, cmd[0], cmd);
	if (!term)
		errExit("openTerm failed.\n");
	initPalette(term, alpha);

	/* Pane作成 */
	win->pane = createPane(&dinfo, xfont, w, h, pad, pad, term);

	return win;
}

void
initPalette(Term *term, float alpha)
{
	char *xrm, *str_type, buf[16];
	XrmDatabase xdb;
	XrmValue val;
	int i;

	/* パレットの設定を読み込む */
	xrm = XResourceManagerString(dinfo.disp);
	xdb = XrmGetStringDatabase(xrm ? xrm : "");
#define XRCOLOR(name, num) do {\
	if (XrmGetResource(xdb, (name), "chitan", &str_type, &val) &&\
	    strncmp(str_type, "String", 6) == 0 &&\
	    strlen(val.addr) ==7 && val.addr[0] == '#')\
		term->palette[num] = strtol(val.addr + 1, NULL, 16) + 0xff000000;\
} while (0)
	XRCOLOR("chitan.foreground", deffg);
	XRCOLOR("chitan.background", defbg);
	for (i = 0; i < 256; i++) {
		snprintf(buf, 16, "chitan.color%d", i);
		XRCOLOR(buf, i);
	}
#undef XRCOLOR
	XrmDestroyDatabase(xdb);

	/* 背景の不透明度を設定 */
	term->palette[defbg] = ((0xff & (int)(0xff * alpha)) << 24) +
		(0x00ffffff &term->palette[defbg]);

	/* 現在のパレットをデフォルトとして保存 */
	for (i = 0; i < PALETTE_SIZE; i++)
		term->def_palette[i] = term->palette[i];
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
setWindowName(Win *win, const char *name)
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
	Pane *pane = win->pane;
	XEvent event;
	const XConfigureEvent *ce = (XConfigureEvent *)&event;
	const XClientMessageEvent *cme = (XClientMessageEvent *)&event;
	int mx, my, ms, mb;

	while (0 < XPending(dinfo.disp)) {
		XNextEvent(dinfo.disp, &event);

		/* IMEのイベントをフィルタリング */
		if (!xim)
			ximOpen(dinfo.disp, NULL, NULL);
		if (xim && XFilterEvent(&event, None) == True)
			continue;

		/* マウス関連 */
		mx = (event.xbutton.x - pane->d.xpad + xfont->cw / 2) / xfont->cw;
		my = (event.xbutton.y - pane->d.ypad) / xfont->ch;
		mb = event.xbutton.button;
		ms = event.xbutton.state;

		switch (event.type) {
		case KeyPress:          /* キーボード入力 */
			if (keyPressEvent(win, event, 64)) {
				pane->d.caret_time = tstons(now);
				pane->d.scr = 0;
			}
			break;

		case ButtonPress:       /* マウス Press */
			if ((mb == 4 || mb == 5) && pane->term->sb == &pane->term->ori) {
				scrollPane(&pane->d, (mb == 4 ? 1 : -1) * 3);
				write(redraw_pipe[1], "a", 1);
			} else if (!BETWEEN(mb, 1, 4) || (ms & ~(ShiftMask | Mod1Mask | Mod2Mask)) ||
					(pane->term->sb == &pane->term->alt && !(ms & ShiftMask))) {
				mouseEvent(pane, &event);
			} else if (mb == 2) {
				XConvertSelection(dinfo.disp, XA_PRIMARY, atoms[UTF8_STRING],
						XA_PRIMARY, win->window, CurrentTime);
				pane->d.scr = 0;
			} else {
				win->dragging = pane;
				selectPane(pane, my, mx, mb == 1, 0 < (ms & Mod1Mask));
				write(redraw_pipe[1], "a", 1);
			}
			break;

		case MotionNotify:     /* マウス Move */
			if (!win->dragging) {
				mouseEvent(pane, &event);
			} else {
				selectPane(win->dragging, my, mx, false, pane->sel.rect);
				write(redraw_pipe[1], "a", 1);
			}
			break;

		case ButtonRelease:    /* マウス Release */
			if (win->dragging && (mb == 1 || mb == 3)) {
				if (win->dragging->sel.aline == win->dragging->sel.bline &&
				    win->dragging->sel.acol  == win->dragging->sel.bcol)
					break;
				XSetSelectionOwner(dinfo.disp, XA_PRIMARY,
						win->window, CurrentTime);
				copySelection(&win->dragging->sel, &win->primary,
						!win->dragging->sel.rect);
				win->dragging = NULL;
			} else {
				mouseEvent(pane, &event);
			}
			break;

		case Expose:            /* 再描画 */
			write(redraw_pipe[1], "a", 1);
			break;

		case ConfigureNotify:   /* ウィンドウサイズ変更 */
			if (win->width != ce->width || win->height != ce->height) {
				win->width  = ce->width;
				win->height = ce->height;
				setPaneSize(&pane->d, ce->width, ce->height);
				setWinSize(pane->term, pane->d.rows, pane->d.cols,
						ce->width, ce->height);
				write(redraw_pipe[1], "a", 1);
			}
			break;

		case FocusIn:
		case FocusOut:          /* フォーカスの変化 */
			pane->d.focus = event.type == FocusIn;
			if (1 < pane->term->dec[1004])
				writePty(pane->term, pane->d.focus ? "\e[I" : "\e[O", 3);
			write(redraw_pipe[1], "a", 1);
			break;

		case ClientMessage:     /* ウィンドウが閉じられた */
			return cme->data.l[0] == atoms[WM_DELETE_WINDOW];

		case SelectionRequest:  /* 貼り付ける文字列を送る */
			sendSelection(win, event);
			break;

		case SelectionNotify:   /* 貼り付ける文字列が届いた */
			receiveSelection(win, pane, event);
			break;
		}
	}

	return 0;
}

int
keyPressEvent(Win *win, XEvent event, int bufsize)
{
	const struct Key { int symbol; char *normal, *app, *normal_m, *app_m;} keys[] = {
		{ XK_Up,        "\e[A",         "\eOA",         "\e[1;%dA",     "\e[1;%dA",     },
		{ XK_Down,      "\e[B",         "\eOB",         "\e[1;%dB",     "\e[1;%dB",     },
		{ XK_Right,     "\e[C",         "\eOC",         "\e[1;%dC",     "\e[1;%dC",     },
		{ XK_Left,      "\e[D",         "\eOD",         "\e[1;%dD",     "\e[1;%dD",     },
		{ XK_Home,      "\e[H",         "\eOH",         "\e[1;%dH",     "\e[1;%dH",     },
		{ XK_End,       "\e[F",         "\eOF",         "\e[1;%dF",     "\e[1;%dF",     },
		{ XK_Page_Up,   "\e[5~",        "\e[5~",        "\e[5;%d~",     "\e[5;%d~",     },
		{ XK_Page_Down, "\e[6~",        "\e[6~",        "\e[6;%d~",     "\e[6;%d~",     },
		{ XK_Insert,    "\e[2~",        "\e[2~",        "\e[2;%d~",     "\e[2;%d~",     },
		{ XK_Delete,    "\e[3~",        "\e[3~",        "\e[3;%d~",     "\e[3;%d~",     },
		{ XK_F1,        "\eOP",         "\eOP",         "\e[1;%dP",     "\e[1;%dP",     },
		{ XK_F2,        "\eOQ",         "\eOQ",         "\e[1;%dQ",     "\e[1;%dQ",     },
		{ XK_F3,        "\eOR",         "\eOR",         "\e[1;%dR",     "\e[1;%dR",     },
		{ XK_F4,        "\eOS",         "\eOS",         "\e[1;%dS",     "\e[1;%dS",     },
		{ XK_F5,        "\e[15~",       "\e[15~",       "\e[15;%d~",    "\e[15;%d~",    },
		{ XK_F6,        "\e[17~",       "\e[17~",       "\e[17;%d~",    "\e[17;%d~",    },
		{ XK_F7,        "\e[18~",       "\e[18~",       "\e[18;%d~",    "\e[18;%d~",    },
		{ XK_F8,        "\e[19~",       "\e[19~",       "\e[19;%d~",    "\e[19;%d~",    },
		{ XK_F9,        "\e[20~",       "\e[20~",       "\e[20;%d~",    "\e[20;%d~",    },
		{ XK_F10,       "\e[21~",       "\e[21~",       "\e[21;%d~",    "\e[21;%d~",    },
		{ XK_F11,       "\e[23~",       "\e[23~",       "\e[23;%d~",    "\e[23;%d~",    },
		{ XK_F12,       "\e[24~",       "\e[24~",       "\e[24;%d~",    "\e[24;%d~",    },
		{ XK_VoidSymbol }
	}, *key;
	char buf[MAX(bufsize, 16)] = {}, *str;
	int len, mod;
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

	/* BackSpace */
	if (keysym == XK_BackSpace && !(event.xkey.state & ControlMask))
		snprintf(buf, 2, "\x7f");

	/* C-S-cまたはC-Insertでコピー */
	if ((keysym == XK_C && event.xkey.state & ControlMask) ||
	    (keysym == XK_Insert && event.xkey.state == ControlMask)) {
		if (win->pane->sel.aline == win->pane->sel.bline &&
		    win->pane->sel.acol  == win->pane->sel.bcol)
			return 0;
		copySelection(&win->pane->sel, &win->clip, !win->pane->sel.rect);
		XSetSelectionOwner(dinfo.disp, atoms[CLIPBOARD], win->window, CurrentTime);
		return 0;
	}

	/* C-S-vまたはS-Insertで貼り付け */
	if ((keysym == XK_V && event.xkey.state & ControlMask) ||
	    (keysym == XK_Insert && event.xkey.state == ShiftMask)) {
		XConvertSelection(dinfo.disp, atoms[CLIPBOARD], atoms[UTF8_STRING],
				atoms[CLIPBOARD], win->window, CurrentTime);
		return 1;
	}

	/* カーソルキー等を送る */
	mod = (event.xkey.state & ShiftMask   ? 1 : 0) +
	      (event.xkey.state & Mod1Mask    ? 2 : 0) +
	      (event.xkey.state & ControlMask ? 4 : 0) +
	      (event.xkey.state & Mod4Mask    ? 8 : 0);
	for (key = keys; key->symbol != XK_VoidSymbol; key++) {
		if (key->symbol == keysym) {
			if (mod == 0) {
				str = win->pane->term->dec[1] < 2 ? key->normal : key->app;
			} else {
				str = win->pane->term->dec[1] < 2 ? key->normal_m : key->app_m;
				snprintf(buf, bufsize, str, mod + 1);
				str = buf;
			}
			writePty(win->pane->term, str, strlen(str));
			return 1;
		}
	}

	/* 文字列を送る */
	if (strlen(buf)) {
		if (event.xkey.state & Mod1Mask)
			writePty(win->pane->term, "\e", 1);
		if (keysym == XK_Escape && 1 < win->pane->term->dec[7727])
			writePty(win->pane->term, "\eO[", 3);
		else
			writePty(win->pane->term, buf, len);
		return 1;
	}

	return 0;
}

void
sendSelection(Win *win, XEvent event)
{
	XSelectionRequestEvent *sre;
	XSelectionEvent se;
	char *sel;

	sre = &event.xselectionrequest;
	sel = sre->selection == XA_PRIMARY ? win->primary : win->clip;
	if (!sel)
		return;
	if (sre->property == None)
		sre->property = sre->target;
	XChangeProperty(dinfo.disp, sre->requestor, sre->property,
			atoms[UTF8_STRING], 8, PropModeReplace, (unsigned char *)sel,
			MIN(strlen(sel), 4 * XMaxRequestSize(dinfo.disp)));
	se = (XSelectionEvent){ SelectionNotify, 0, True, dinfo.disp,
		sre->requestor, sre->selection, sre->target, sre->property, sre->time };
	XSendEvent(dinfo.disp, event.xselectionrequest.requestor, False, 0, (XEvent *)&se);
}

void
receiveSelection(Win *win, Pane *pane, XEvent event)
{
	Atom prop, type;
	int format;
	unsigned long ntimes, after;
	unsigned char *props;
	int res;

	prop = event.xselection.property;
	if (prop == None)
		return;
	res = XGetWindowProperty(dinfo.disp, win->window, prop, 0, 2 << 14, False,
			atoms[UTF8_STRING], &type, &format, &ntimes, &after, &props);
	if (res != Success)
		return;
	if (1 < pane->term->dec[2004])
		writePty(pane->term, "\e[200~", 6);
	writePty(pane->term, (char *)props, ntimes);
	if (1 < pane->term->dec[2004])
		writePty(pane->term, "\e[201~", 6);
	XFree(props);
}

void
redraw(Win *win)
{
	setWindowName(win, win->pane->term->title);
	if (drawPane(&win->pane->d, tstons(now), win->ime.peline, win->ime.caret)) {
		XCopyArea(dinfo.disp, win->pane->d.pixmap, win->window, win->gc,
				0, 0, win->pane->d.width, win->pane->d.height, 0, 0);
		XFlush(dinfo.disp);
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
	write(redraw_pipe[1], "a", 1);
}

void
preeditDraw(XIM xim, Win *win, XIMPreeditDrawCallbackStruct *call)
{
	char32_t *str;
	int attr;
	int len, oldlen = u32slen(win->ime.peline->str);
	XIMFeedback fb;
	int i;

	/* カーソル位置 */
	win->ime.caret = call->caret;

	/* 削除の処理 */
	deleteChars(win->ime.peline, call->chg_first, call->chg_length);

	if (call->text == NULL)
		return;

	/* 挿入の準備 */
	len  = call->text->length;
	str  = xmalloc(len * sizeof(char32_t));
	u8sToU32s(str, call->text->string.multi_byte, len);
	attr = ULINE;
	if (0 < oldlen)
		attr = win->ime.peline->attr[MIN(call->chg_first, oldlen - 1)];

	/* 挿入を実行 */
	for (i = 0; i < len; i++) {
		if (call->text->feedback) {
			fb = call->text->feedback[i];
			attr  = NONE;
			attr |= fb & XIMReverse   ? NEGA    : NONE;
			attr |= fb & XIMUnderline ? ULINE   : NONE;
			attr |= fb & XIMHighlight ? BOLD    : NONE;
		}
		insertU32s(win->ime.peline, call->chg_first + i,
				str + i, attr, deffg, defbg, 1);
	}

	/* 終了 */
	free(str);

	write(redraw_pipe[1], "a", 1);
}

void
preeditCaret(XIM xim, Win *win, XIMPreeditCaretCallbackStruct *call)
{
	win->ime.caret = call->position;
	write(redraw_pipe[1], "a", 1);
}
