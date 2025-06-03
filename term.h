#include <sys/types.h>

#include "line.h"

/* 端末 */
typedef struct Term {
	int master;     /* 疑似端末のファイルディスクリプタ */
	Line **lines;   /* バッファ */
	int maxlines;   /* バッファの最大行数*/
	int lastline;   /* 今の最終行 */
	int cursor;     /* カーソル位置 */
} Term;

Term *openTerm(void);
void closeTerm(Term *);
ssize_t readPty(Term *);
ssize_t writePty(Term *, char *, ssize_t);
