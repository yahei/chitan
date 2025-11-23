#include <stdint.h>

typedef uint_least32_t char32_t;
typedef uint_least32_t Color;

extern Color deffg, defbg;
extern const Color PALETTE_SIZE;

#define PUT_NUL(l, x)   putU32s((l), (x), (char32_t *)L"\0", 0, deffg, defbg, 1)
#define u32swidth(s)    u32snwidth(s, u32slen(s))

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
	int *attr;
	Color *fg, *bg;
	int ver;
} Line;

typedef struct InsertLine {
	const char32_t *str;
	const int *attr;
	const Color *fg, *bg;
} InsertLine;

typedef struct CharCnt {
	int index;
	int col;
	int width;
} CharCnt;

Line *allocLine(void);
void freeLine(Line *);
void linecpy(Line *, const Line *);
int linecmp(Line *, Line *, int, int);
void insertU32s(Line *, int, const InsertLine *, int);
void deleteChars(Line *, int, int);
int eraseInLine(Line *, int, int);
int putU32s(Line *, int, const char32_t *, int, Color, Color, size_t);
void putSPCs(Line *, int, Color, size_t);
int findNextSGR(const Line *, int);

const char *u8sToU32s(char32_t *,const char *, size_t);
size_t u32slen(const char32_t *);
int u32snwidth(const char32_t *, int);
CharCnt getCharCnt(const char32_t *, int);
int getIndex(const char32_t *, int);
