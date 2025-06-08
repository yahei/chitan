#include <stdint.h>

typedef uint_least32_t char32_t;

typedef struct Line {
	char32_t *str;
} Line;

Line *allocLine(void);
void freeLine(Line *);
void insertU8(Line *, int, const char *, int);
void deleteChars(Line *, int, int);
int putU8(Line *, int, const char *, int);

int u8sToU32s(const char *, char32_t *, int);
int u32slen(const char32_t *);
int u32swidth(const char32_t *, int);
int u32slencol(const char32_t *, int);
