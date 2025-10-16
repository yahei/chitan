#include <stdint.h>
#include <stdlib.h>

extern int deffg;
extern int defbg;

typedef uint_least32_t char32_t;

#define PUT_NUL(l,x)    (putU32s(l, x, (char32_t *)L"\0", 0, deffg, defbg, 1))

enum sgr_attribute {
	NONE    = 0,
	BOLD    = 1 << 0,   /* 太字 */
	FAINT   = 1 << 1,   /* 細字 */
	ITALIC  = 1 << 2,   /* 斜体 */
	ULINE   = 1 << 3,   /* 下線 */
	BLINK   = 1 << 4,   /* 点滅 */
	RAPID   = 1 << 5,   /* 高速点滅 */
	NEGA    = 1 << 6,   /* 反転 */
	CONCEAL = 1 << 7,   /* 非表示 */
	STRIKE  = 1 << 8,   /* 取消 */
	DULINE  = 1 << 9    /* 二重下線 */
};

typedef struct Line {
	char32_t *str;
	int *attr, *fg, *bg;
} Line;

typedef struct InsertLine {
	const char32_t *str;
	const int *attr, *fg, *bg;
} InsertLine;

typedef struct CharCnt {
	int index;
	int col;
	int width;
} CharCnt;

Line *allocLine(void);
void freeLine(Line *);
void insertU32s(Line *, int, const InsertLine *, int);
void deleteChars(Line *, int, int);
int eraseInLine(Line *, int, int);
int putU32s(Line *, int, const char32_t *, int, int, int, size_t);
void putSPCs(Line *, int, int, size_t);

const char *u8sToU32s(char32_t *,const char *, size_t);
size_t u32slen(const char32_t *);
int u32swidth(const char32_t *, int);
CharCnt getCharCnt(const char32_t *, int);
int findNextSGR(const Line *, int);
