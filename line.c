#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "line.h"
#include "util.h"

/*
 * Line
 *
 * ログ1行を管理する
 */

Line *
allocLine(void)
{
	Line *line = xmalloc(sizeof(Line));

	line->str = xmalloc(sizeof(char32_t));
	line->str[0] = L'\0';

	return line;
}

void
freeLine(Line *line)
{
	if (line == NULL)
		return;

	free(line->str);
	free(line);
}

void
insertU32(Line *line, int head, const char32_t *str, int len)
{
	const int oldlen = u32slen(line->str);
	const int newlen = oldlen + len + 1;
	const int movelen = MAX(oldlen - head, 0) + 1;

	if (head < 0 || len <= 0)
		return;

	line->str = xrealloc(line->str, newlen * sizeof(char32_t));
	memmove(&line->str[head + len], &line->str[MIN(head, oldlen)],
			movelen * sizeof(char32_t));
	memcpy(&line->str[head], str, len * sizeof(char32_t));
}

void
deleteChars(Line *line, int head, int len)
{
	const int oldlen = u32slen(line->str);
	char32_t *newstr;
	int tail = MIN(head + len, oldlen);

	if (head < 0 || tail <= head)
		return;

	newstr = xmalloc((oldlen - (tail - head) + 1) * sizeof(char32_t));
	memcpy(newstr, line->str, head * sizeof(char32_t));
	memcpy(&newstr[head], &line->str[tail],
			(oldlen - tail + 1) * sizeof(char32_t));

	free(line->str);
	line->str = newstr;
}

int
eraseInLine(Line *line, int col, int width)
{
	const int linelen = u32slen(line->str);
	int head, tail;
	int lpad, rpad;
	CharCnt cc;
	int i;

	if (col < 0)
		return 0;

	cc = getCharCnt(line, col);
	head = MIN(cc.index, linelen);
	lpad = col - MIN(cc.col, u32swidth(line->str, linelen));

	cc = getCharCnt(line, col + width - 1);
	tail = MIN(cc.index, linelen) + 1;
	rpad = (cc.col + cc.width) - (col + width);

	for (; tail < linelen; tail++)
		if(0 < u32swidth(&line->str[tail], 1))
			break;

	deleteChars(line, head, tail - head);

	for (i = 0; i < rpad + lpad; i++)
		insertU32(line, head, (char32_t *)L" ", 1);

	return head + lpad;
}

int
putU32(Line *line, int col, const char32_t *str, int len)
{
	const int width = u32swidth(str, len);
	int head;

	if (col < 0)
		return 0;

	head = eraseInLine(line, col, width);
	insertU32(line, head, str, len);

	return width;
}

void
deleteTrail(Line *line)
{
	int i;

	for (i = u32slen(line->str); 0 < i; i--)
		if (line->str[i - 1] != L' ')
			break;
	line->str[i] = L'\0';
}

CharCnt
getCharCnt(const Line *line, int col)
{
	const int linelen = u32slen(line->str);
	int width, total;
	int i;

	if (col < 0)
		return (CharCnt){col, col, 1};

	for (i = 0, total = 0; i < linelen; i++) {
		width = wcwidth(line->str[i]);
		width = width < 0 ? 2 : width;
		if (col < total + width)
			return(CharCnt){i, total, width};
		total += width;
	}

	return (CharCnt){linelen + (col - total), col, 1};
}

const char *
u8sToU32s(char32_t *dst, const char *src, size_t n)
{
	const char *rest = src;
	int bytes;

	for (;;) {
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

int
u32slen(const char32_t *str)
{
	return wcslen((const wchar_t *)str);
}

int
u32swidth(const char32_t *str, int len)
{
	int width, total;
	int i;

	for (i = total = 0; i < len && str[i] != L'\0'; i++) {
		width = wcwidth(str[i]);
		total += width < 0 ? 2 : width;
	}

	return total;
}
