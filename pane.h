#include <stdbool.h>
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
	Pixmap pixmap, pixbuf;
	Line **lines;
	unsigned int depth;
	float alpha;
	GC gc;
	XftDraw *draw;
	int width, height, xpad, ypad;
	bool focus, redraw_flag;
	Term *term;
	int scr, prevfst;
	struct ScreenBuffer *prevbuf;
	struct Selection {
		int aline, acol, bline, bcol;
		int rect, altbuf;
		char *primary, *clip;
		int *vers;
	} sel;
	struct timespec timers[TIMER_NUM], now;
	bool timer_active[TIMER_NUM];
	bool timer_lit[TIMER_NUM], bell_b;
} Pane;

Pane *createPane(DispInfo *, XFont *, int, int, float, int, char *const []);
void destroyPane(Pane *);
void setPaneSize(Pane *, int, int);
void mouseEvent(Pane *, XEvent *);
void scrollPane(Pane *, int);
void setSelection(Pane *, int, int, char, char);
void checkSelection(Pane *);
void copySelection(Pane *, char **, int);
void manegeTimer(Pane *, struct timespec *);
void drawPane(Pane *, Line *, int);
