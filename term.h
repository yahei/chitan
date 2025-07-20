#include "line.h"

typedef uint_least32_t Color;
#define RED(c)   (c >> 16 & 0xff)
#define GREEN(c) (c >>  8 & 0xff)
#define BLUE(c)  (c >>  0 & 0xff)

#define GETOPT(a,n)     (0 < (a[n / 8] & 1 << (n % 8)))

enum mouse_event_type {
	SHIFT   = 4,
	ALT     = 8,
	CTRL    = 16,
	MOVE    = 32,
	WHEEL   = 64,
	OTHER   = 128
};

/* 端末 */
typedef struct Term {
	int master;     /* 疑似端末のファイルディスクリプタ */
	struct ScreenBuffer {
		Line **lines;   /* ログ */
		int maxlines;   /* ログの最大行数*/
		int lastline;   /* ログの最終行 */
		int rows, cols; /* 画面の行数と列数 */
		int scrs, scre; /* スクロール範囲 */
	} ori, alt, *sb;
	int cx, cy;             /* カーソル位置 */
	int svx, svy;           /* 保存したカーソル位置 */
	int ctype;              /* カーソル形状 */
	char *readbuf;          /* 可変長リードバッファ */
	int rblen;              /* リードバッファに残っている文字の数 */
	char opt[8];            /* オプション */
	char dec[1100];         /* 拡張オプション */
	int attr, fg, bg;       /* 現在のSGR */
	Color *palette;         /* カラーパレット */
	int oldmx, oldmy;       /* 前回のマウス座標 */
} Term;

Term *openTerm(void);
void closeTerm(Term *);
ssize_t readPty(Term *);
ssize_t writePty(Term *, const char *, ssize_t);
Line *getLine(Term *, int);
void setWinSize(Term *, int, int, int, int);
void reportMouse(Term *, int, int, int, int);
