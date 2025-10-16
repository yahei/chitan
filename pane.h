#include <time.h>

#include "term.h"
#include "font.h"

enum timer_names { BELL_TIMER, BLINK_TIMER, RAPID_TIMER, CARET_TIMER, TIMER_NUM };

typedef struct Pane {
	Display *disp;
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
		int rect, altbuf, dragging;
		char *primary, *clip;
	} selection;
	struct timespec timers[TIMER_NUM], now;
	char timer_active[TIMER_NUM];
	char timer_lit[TIMER_NUM];
} Pane;

Pane *createPane(Display *, XFont *, Drawable, int, int, int);
void destroyPane(Pane *);
void setPaneSize(Pane *, int, int);

void mouseEvent(Pane *, XEvent *);
void scrollPane(Pane *, int);
void copySelection(Pane *, char **);
void manegeTimer(Pane *, struct timespec *);

void drawPane(Pane *, Line *, int);
void drawLine(Pane *, Line *, int, int, int, int);
void drawCursor(Pane *, Line *, int, int, int);
void drawSelection(Pane *, struct Selection *);
void drawLineRev(Pane *, Line *, int, int, int);

void bell(void *);
