#include "line.h"

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
	int cx, cy;     /* カーソル位置 */
	int svx, svy;   /* 保存したカーソル位置 */
	char *readbuf;  /* 可変長リードバッファ */
	int rblen;      /* リードバッファに残っている文字の数 */
	char opt[8];    /* オプション */
	char dec[1100]; /* 拡張オプション */
} Term;

Term *openTerm(void);
void closeTerm(Term *);
ssize_t readPty(Term *);
ssize_t writePty(Term *, const char *, ssize_t);
Line *getLine(Term *, int);
void setWinSize(Term *, int, int, int, int);
