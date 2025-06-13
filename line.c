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
putU32(Line *line, int col, const char32_t *str, int len)
{
	const int linelen = u32slen(line->str);
	const int width = u32swidth(str, len);
	int head, tail;
	int lpad, rpad;

	if (col < 0)
		return 0;

	for (head = 0; head < linelen; head++)
		if (col < u32swidth(line->str, head + 1))
			break;
	lpad = col - u32swidth(line->str, head);

	for (tail = head; tail < linelen; tail++)
		if (col + width <= u32swidth(line->str, tail))
			break;
	rpad = u32swidth(line->str, tail) - (col + width);

	for (; tail < linelen; tail++)
		if(0 < u32swidth(&line->str[tail], 1))
			break;

	head += lpad;
	tail += lpad;
	for (; 0 < rpad; rpad--)
		insertU32(line, tail - lpad, (char32_t *)L" ", 1);
	for (; 0 < lpad; lpad--)
		insertU32(line, head - lpad, (char32_t *)L" ", 1);

	deleteChars(line, head, tail - head);
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

int
u32slencol(const char32_t *str, int col)
{
	const int linelen = u32slen(str);
	int len;

	if (col < 0)
		return -col;

	for (len = 0; len < linelen; len++)
		if (col < u32swidth(str, len + 1))
			return len;

	return linelen + col - u32swidth(str, linelen);
}
