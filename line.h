/* 字 */
typedef unsigned int utf32;

/* 行 */
typedef struct Line {
	utf32 *str;
} Line;

Line *allocLine(void);
void freeLine(Line *);
void insertUtf8(Line *, int, const char *, int);
void deleteChars(Line *, int, int);
void putUtf8(Line *, int, const char *, int);

/* 文字列の操作 */
void utf8sToUtf32s(const char *, utf32 *, int);
int utf32slen(const utf32 *);
int utf32swidth(const utf32 *, size_t);
int utf32sposlen(const utf32 *, int);
