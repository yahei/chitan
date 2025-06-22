#include <stdint.h>

typedef uint_least32_t char32_t;

typedef struct Line {
	char32_t *str;
} Line;

typedef struct CharCnt {
	int index;
	int col;
	int width;
} CharCnt;

Line *allocLine(void);
void freeLine(Line *);
void insertU32(Line *, int, const char32_t *, int);
void deleteChars(Line *, int, int);
int putU32(Line *, int, const char32_t *, int);
void deleteTrail(Line *);
CharCnt getCharCnt(const Line *, int);

const char *u8sToU32s(char32_t *,const char *, size_t);
int u32slen(const char32_t *);
int u32swidth(const char32_t *, int);
