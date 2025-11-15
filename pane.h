#include <stdbool.h>
#include <time.h>

#include "term.h"
#include "font.h"

typedef long long int   nsec;
#define GIGA            (1000000000)
#define tstons(t)       ((long long)(t).tv_sec * GIGA + (t).tv_nsec)
#define nstots(t)       ((struct timespec){ (t) / GIGA, (t) % GIGA })

typedef struct DispInfo {
	Display *disp;
	int screen;
	Window root;
	Visual *visual;
	Colormap cmap;
} DispInfo;

enum timer_names { BLINK_TIMER, RAPID_TIMER, CARET_TIMER, TIMER_NUM };

typedef struct Pane {
	DispInfo *dinfo;
	XFont *xfont;
	Pixmap pixmap, pixbuf;
	Line **lines, **lines_b;
	nsec time_b;
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
	nsec caret_time, bell_time;
	bool timer_active[TIMER_NUM];
	int bell_cnt, pallet_cnt;
} Pane;

Pane *createPane(DispInfo *, XFont *, int, int, float, int, char *const []);
void destroyPane(Pane *);
void setPaneSize(Pane *, int, int);
void mouseEvent(Pane *, XEvent *);
void scrollPane(Pane *, int);
void selectPane(Pane *, int, int, bool, bool);
nsec getNextTime(Pane *, nsec);
int drawPane(Pane *, nsec, Line *, int);
