#include <time.h>

#include "term.h"
#include "font.h"

typedef struct DispInfo {
	Display *disp;
	Visual *visual;
	Colormap cmap;
} DispInfo;

enum timer_names { BELL_TIMER, BLINK_TIMER, RAPID_TIMER, CARET_TIMER, TIMER_NUM };

typedef struct Pane {
	DispInfo *dinfo;
	XFont *xfont;
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
		int rect, altbuf;
		char *primary, *clip;
	} sel;
	struct timespec timers[TIMER_NUM], now;
	char timer_active[TIMER_NUM];
	char timer_lit[TIMER_NUM];
} Pane;

Pane *createPane(DispInfo *, XFont *, Drawable, int, int, int);
void destroyPane(Pane *);
void setPaneSize(Pane *, int, int);
void mouseEvent(Pane *, XEvent *);
void scrollPane(Pane *, int);
void setSelection(Pane *, int, int, char, char);
void copySelection(Pane *, char **, int);
void manegeTimer(Pane *, struct timespec *);
void drawPane(Pane *, Line *, int);
