#include <stdlib.h>
#include <string.h>

#include "line.h"
#include "util.h"

/*
 * Line
 *
 * バッファ1行を管理する
 */

Color deffg = 256, defbg = 257;
const Color PALETTE_SIZE = 258;

Line *
allocLine(void)
{
	Line *line = xmalloc(sizeof(Line));

	line->str  = xmalloc(sizeof(char32_t));
	line->attr = xmalloc(sizeof(int));
	line->fg   = xmalloc(sizeof(int));
	line->bg   = xmalloc(sizeof(int));

	line->str[0]  = L'\0';
	line->attr[0] = NONE;
	line->fg[0]   = deffg;
	line->bg[0]   = defbg;

	return line;
}

void
freeLine(Line *line)
{
	if (line == NULL)
		return;

	free(line->str);
	free(line->attr);
	free(line->fg);
	free(line->bg);
	free(line);
}

void
linecpy(Line *dst, const Line *src)
{
	size_t len = u32slen(src->str) + 1;

	dst->str  = xrealloc(dst->str,  len * sizeof(char32_t));
	dst->attr = xrealloc(dst->attr, len * sizeof(int));
	dst->fg   = xrealloc(dst->fg,   len * sizeof(int));
	dst->bg   = xrealloc(dst->bg,   len * sizeof(int));

	memcpy(dst->str,  src->str,  len * sizeof(char32_t));
	memcpy(dst->attr, src->attr, len * sizeof(int));
	memcpy(dst->fg,   src->fg,   len * sizeof(int));
	memcpy(dst->bg,   src->bg,   len * sizeof(int));
}

int
linecmp(Line *line1, Line *line2, int pos, int len)
{
	int index1, col1, width1;
	int index2, col2, width2;

	getCharCnt(line1->str, pos, &index1, &col1, &width1);
	getCharCnt(line2->str, pos, &index2, &col2, &width2);

#define CMP(A,T) !memcmp(&line1->A[index1], &line2->A[index2], len * sizeof(T))
	if (col1 == col2 && index2 + len <= u32slen(line2->str) &&
	    CMP(str, char32_t) && CMP(attr, int) && CMP(fg, int) && CMP(bg, int))
			return 1;
	return 0;
#undef CMP
}

void
insertU32s(Line *line, int head, const char32_t *str, int attr, Color fg, Color bg, int len)
{
	const int oldlen = u32slen(line->str);
	const int newlen = oldlen + len;
	const int movelen = MAX(oldlen - head, 0);
	int i;

	if (head < 0 || len <= 0)
		return;

#define INSERT(d, s) do { \
	d = xrealloc(d, (newlen + 1) * s); \
	memmove(&d[head + len], &d[MIN(head, oldlen)], (movelen + 1) * s); \
} while (0);
	INSERT(line->str,   sizeof(char32_t));
	INSERT(line->attr,  sizeof(int));
	INSERT(line->fg,    sizeof(Color));
	INSERT(line->bg,    sizeof(Color));
#undef INSERT

	memcpy(&line->str[head], str, len * sizeof(char32_t));
	for (i = 0; i < len; i++) {
		line->attr[head + i] = attr;
		line->fg[head + i] = fg;
		line->bg[head + i] = bg;
	}

	line->ver++;
}

void
deleteChars(Line *line, int head, int len)
{
	const int oldlen = u32slen(line->str);
	const int tail = MIN(head + len, oldlen);

	if (head < 0 || tail <= head)
		return;

#define DELETE(target, size) do { \
	memmove(&target[head], &target[tail], (oldlen - tail + 1) * size); \
	target = xrealloc(target, (head + oldlen - tail + 1) * size); \
} while (0);
	DELETE(line->str,   sizeof(char32_t));
	DELETE(line->attr,  sizeof(int));
	DELETE(line->fg,    sizeof(Color));
	DELETE(line->bg,    sizeof(Color));
#undef DELETE

	line->ver++;
}

int
eraseInLine(Line *line, int col, int width)
{
	const int attr = 0;
	const int linelen = u32slen(line->str);
	int head, tail;
	int lpad, rpad;
	int index, c, w;

	if (col < 0)
		return 0;

	getCharCnt(line->str, col, &index, &c, &w);
	head = MIN(index, linelen);
	lpad = col - MIN(c, u32snwidth(line->str, linelen));

	getCharCnt(line->str, col + width - 1, &index, &c, &w);
	tail = MIN(index, linelen) + 1;
	rpad = (c + w) - (col + width);

	for (; tail < linelen; tail++)
		if(0 < u32snwidth(&line->str[tail], 1))
			break;

	deleteChars(line, head, tail - head);

	if (0 < rpad + lpad) {
		char32_t str[rpad + lpad];
		INIT(str, L' ');
		insertU32s(line, head, str, attr, deffg, defbg, rpad + lpad);
	}

	return head + lpad;
}

int
putU32s(Line *line, int col, const char32_t *str, int attr, Color fg, Color bg, size_t len)
{
	const int width = u32snwidth(str, len);
	int head;

	if (col < 0)
		return 0;

	head = eraseInLine(line, col, width);
	insertU32s(line, head, str, attr, fg, bg, len);

	return width;
}

void
putSPCs(Line *line, int col, Color bg, size_t n)
{
	char32_t str[n] = {};

	INIT(str, L' ');
	putU32s(line, col, str, 0, deffg, bg, n);
}

int
findNextSGR(const Line *line, int index)
{
	const int len = u32slen(line->str);

	if (index < len) {
		const int attr = line->attr[index];
		const int fg   = line->fg[index];
		const int bg   = line->bg[index];

		for (; index < len; index++)
			if (attr != line->attr[index] ||
			    fg   != line->fg[index] ||
			    bg   != line->bg[index])
				return index;
	}

	return len;
}

const char *
u8sToU32s(char32_t *dst, const char *src, size_t n)
{
	const char *rest = src;
	int bytes;

	while (1) {
		if (n == 0)
			return rest;

		if ((0 <= *src && *src < 32) || *src == 127)
			break;

		bytes = mbtowc((wchar_t *)dst, src, 4);

		if (bytes < 0)
			break;

		dst++;
		n--;
		src += bytes;
		rest = src;
	}

	*dst = L'\0';
	return rest;
}

void
getCharCnt(const char32_t *str, int col, int *index, int *total, int *width)
{
	if (col < 0) {
		*index = *total = col;
		*width = 1;
		return;
	}

	for (*index = 0, *total = 0; str[*index] != L'\0'; (*index)++) {
		*width = wcwidth(str[*index]);
		if (col < *total + *width)
			return;
		*total += *width;
	}

	*index = *index + col - *total;
	*total = col;
	*width = 1;
	return;
}

int
getIndex(const char32_t *str, int col)
{
	int total;
	int i;

	if (col < 0)
		return col;

	for (i = 0, total = 0; str[i] != L'\0'; i++) {
		total += wcwidth(str[i]);
		if (col < total)
			return i;
	}

	return i + col - total;
}
