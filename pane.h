#include <stdbool.h>
#include <time.h>

#include "font.h"
#include "term.h"

typedef long long int   nsec;
#define GIGA            (1000000000)
#define tstons(t)       ((long long)(t).tv_sec * GIGA + (t).tv_nsec)
#define nstots(t)       (struct timespec){ (t) / GIGA, (t) % GIGA }

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
	unsigned int depth;
	GC gc;
	XftDraw *draw;
	int width, height, xpad, ypad;
	bool focus, redraw_flag;
	nsec time_b;
	nsec caret_time, bell_time;
	bool timer_active[TIMER_NUM];
	Term *term;
	Line **new_lines, **old_lines;
	Selection sel;
	struct ScrBuf *prevbuf;
	int scr, prevfst;
	int clear_x, clear_y, clear_w, clear_h;
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
