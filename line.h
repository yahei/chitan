#include <stdint.h>

/* 字 */
typedef uint_least32_t char32_t;

/* 行 */
typedef struct Line {
	char32_t *str;
} Line;

Line *allocLine(void);
void freeLine(Line *);
void insertU8(Line *, int, const char *, int);
void deleteChars(Line *, int, int);
int putU8(Line *, int, const char *, int);

/* 文字列の操作 */
void u8sToU32s(const char *, char32_t *, int);
int u32slen(const char32_t *);
int u32swidth(const char32_t *, int);
int u32sposlen(const char32_t *, int);
