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

int deffg = 256;
int defbg = 257;

Line *
allocLine(void)
{
	Line *line = xmalloc(sizeof(Line));

	line->str = xmalloc(sizeof(char32_t));
	line->attr = xmalloc(0);
	line->fg = xmalloc(0);
	line->bg = xmalloc(0);
	line->str[0] = L'\0';

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
insertU32s(Line *line, int head, const InsertLine *ins, int len)
{
	const int oldlen = u32slen(line->str);
	const int newlen = oldlen + len;
	const int movelen = MAX(oldlen - head, 0);

	if (head < 0 || len <= 0)
		return;

#define INSERT(dest, src, size, sent) do { \
	dest = xrealloc(dest, (newlen + sent) * size); \
	memmove(&dest[head + len], &dest[MIN(head, oldlen)], \
			(movelen + sent) * size); \
	memcpy(&dest[head], src, len * size); \
} while (0);

	INSERT(line->str, ins->str, sizeof(char32_t), 1);
	INSERT(line->attr, ins->attr, sizeof(int), 0);
	INSERT(line->fg, ins->fg, sizeof(int), 0);
	INSERT(line->bg, ins->bg, sizeof(int), 0);

#undef INSERT
}

void
deleteChars(Line *line, int head, int len)
{
	const int oldlen = u32slen(line->str);
	char32_t *strbuf;
	int *buf;
	int tail = MIN(head + len, oldlen);

	if (head < 0 || tail <= head)
		return;

#define DELETE(target, buf, size, sent) do { \
	buf = xmalloc((oldlen - (tail - head) + sent) * size); \
	memcpy(buf, target, head * size); \
	memcpy(&buf[head], &target[tail], (oldlen - tail + sent) * size); \
	free(target); \
	target = buf; \
} while (0);

	DELETE(line->str, strbuf, sizeof(char32_t), 1);
	DELETE(line->attr, buf, sizeof(int), 0);
	DELETE(line->fg, buf, sizeof(int), 0);
	DELETE(line->bg, buf, sizeof(int), 0);

#undef DELETE
}

int
eraseInLine(Line *line, int col, int width)
{
	const int attr = 0;
	const InsertLine space = {(char32_t *)L" ", &attr, &deffg, &defbg};
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
		insertU32s(line, head, &space, 1);

	return head + lpad;
}

int
putU32s(Line *line, int col, const char32_t *str, int attr, int fg, int bg, int len)
{
	const int width = u32swidth(str, len);
	int attrs[len], fgs[len], bgs[len];
	int head;
	InsertLine placed;
	int i;

	if (col < 0)
		return 0;

	for (i = 0; i < len; i++) {
		attrs[i] = attr;
		fgs[i] = fg;
		bgs[i] = bg;
	}
	placed = (InsertLine){ str, attrs, fgs, bgs };

	head = eraseInLine(line, col, width);
	insertU32s(line, head, &placed, len);

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
		return (CharCnt){ col, col, 1 };

	for (i = 0, total = 0; i < linelen; i++) {
		width = wcwidth(line->str[i]);
		width = width < 0 ? 2 : width;
		if (col < total + width)
			return (CharCnt){ i, total, width };
		total += width;
	}

	return (CharCnt){ linelen + (col - total), col, 1 };
}

int
findNextSGR(const Line *line, int index)
{
	const int len = u32slen(line->str);

	if (len <= index)
		return len;

	const int attr = line->attr[index];
	const int fg = line->fg[index];
	const int bg = line->bg[index];

	for (; index < len; index++)
		if (attr != line->attr[index] ||
		    fg != line->fg [index] ||
		    bg != line->bg [index])
			return index;

	return len;
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
