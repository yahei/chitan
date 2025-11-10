#include <stdbool.h>
#include <time.h>

#include "term.h"
#include "font.h"

typedef struct DispInfo {
	Display *disp;
	Visual *visual;
	Colormap cmap;
} DispInfo;

enum timer_names { BLINK_TIMER, RAPID_TIMER, CARET_TIMER, TIMER_NUM };

typedef struct Pane {
	DispInfo *dinfo;
	XFont *xfont;
	Pixmap pixmap, pixbuf;
	Line **lines, **lines_b;
	int cx_b, cy_b, cw_b, ch_b;
	unsigned int depth;
	float alpha;
	GC gc;
	XftDraw *draw;
	int width, height, xpad, ypad;
	bool focus, redraw_flag;
	Term *term;
	int scr, prevfst;
	struct ScrBuf *prevbuf;
	Selection sel;
	struct timespec timers[TIMER_NUM], bell_time;
	bool timer_active[TIMER_NUM];
	bool timer_lit[TIMER_NUM], bell_lit;
	int bell_cnt, pallet_cnt;
} Pane;

Pane *createPane(DispInfo *, XFont *, int, int, float, int, char *const []);
void destroyPane(Pane *);
void setPaneSize(Pane *, int, int);
void mouseEvent(Pane *, XEvent *);
void scrollPane(Pane *, int);
void selectPane(Pane *, int, int, bool, bool);
void manageTimer(Pane *, const struct timespec *);
void getNextTime(Pane *, struct timespec *, const struct timespec *);
void drawPane(Pane *, const struct timespec *, Line *, int);
