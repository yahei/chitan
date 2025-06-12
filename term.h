#include "line.h"

/* 端末 */
typedef struct Term {
	int master;     /* 疑似端末のファイルディスクリプタ */
	Line **lines;   /* ログ */
	int maxlines;   /* ログの最大行数*/
	int lastline;   /* ログの最終行 */
	int cursor;     /* カーソル位置 */
	char *readbuf;  /* 可変長リードバッファ */
	int rblen;      /* リードバッファに残っている文字の数 */
} Term;

Term *openTerm(void);
void closeTerm(Term *);
ssize_t readPty(Term *);
ssize_t writePty(Term *, char *, ssize_t);
Line * getLine(Term *, unsigned int);
