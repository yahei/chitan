#include <sys/types.h>

typedef struct Term Term;
typedef struct Line Line;

/* 端末 */
struct Term {
	int master;     /* 疑似端末のファイルディスクリプタ */
	Line **lines;   /* バッファ */
	int maxlines;   /* バッファの最大行数*/
	int lastline;   /* 今の最終行 */
	int cursor;     /* カーソル位置 */
};

Term *openTerm(void);
void closeTerm(Term *);
ssize_t readPty(Term *);
ssize_t writePty(Term *, char *, ssize_t);

/* 行 */
Line *newLine(void);
void deleteLine(Line *);
void setUtf8(Line *, char *, int);
const char *getUtf8(Line *);
void overwriteUtf8(Line *, char *, int, int);
