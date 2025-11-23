#include <stdlib.h>
#include <string.h>
#include <wchar.h>

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
	CharCnt cc1 = getCharCnt(line1->str, pos);
	CharCnt cc2 = getCharCnt(line2->str, pos);

#define CMP(A,T) !memcmp(&line1->A[cc1.index], &line2->A[cc2.index], len * sizeof(T))
	if (cc2.index + len <= u32slen(line2->str) && cc1.col == cc2.col &&
	    CMP(str, char32_t) && CMP(attr, int) && CMP(fg, int) && CMP(bg, int) &&
	    (cc2.index < 1 || !(line2->attr[cc2.index - 1] & ITALIC)))
			return 1;
	return 0;
#undef CMP
}

void
insertU32s(Line *line, int head, const InsertLine *ins, int len)
{
	const int oldlen = u32slen(line->str);
	const int newlen = oldlen + len;
	const int movelen = MAX(oldlen - head, 0);

	if (head < 0 || len <= 0)
		return;

#define INSERT(dest, src, size) do { \
	dest = xrealloc(dest, (newlen + 1) * size); \
	memmove(&dest[head + len], &dest[MIN(head, oldlen)], \
			(movelen + 1) * size); \
	memcpy(&dest[head], src, len * size); \
} while (0);
	INSERT(line->str,   ins->str,   sizeof(char32_t));
	INSERT(line->attr,  ins->attr,  sizeof(int));
	INSERT(line->fg,    ins->fg,    sizeof(Color));
	INSERT(line->bg,    ins->bg,    sizeof(Color));
#undef INSERT
	line->ver++;
}

void
deleteChars(Line *line, int head, int len)
{
	const int oldlen = u32slen(line->str);
	char32_t *strbuf;
	int *abuf;
	Color *buf;
	int tail = MIN(head + len, oldlen);

	if (head < 0 || tail <= head)
		return;

#define DELETE(target, buf, size) do { \
	buf = xmalloc((oldlen - (tail - head) + 1) * size); \
	memcpy(buf, target, head * size); \
	memcpy(&buf[head], &target[tail], (oldlen - tail + 1) * size); \
	free(target); \
	target = buf; \
} while (0);
	DELETE(line->str,   strbuf,     sizeof(char32_t));
	DELETE(line->attr,  abuf,       sizeof(int));
	DELETE(line->fg,    buf,        sizeof(Color));
	DELETE(line->bg,    buf,        sizeof(Color));
#undef DELETE
	line->ver++;
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

	cc = getCharCnt(line->str, col);
	head = MIN(cc.index, linelen);
	lpad = col - MIN(cc.col, u32snwidth(line->str, linelen));

	cc = getCharCnt(line->str, col + width - 1);
	tail = MIN(cc.index, linelen) + 1;
	rpad = (cc.col + cc.width) - (col + width);

	for (; tail < linelen; tail++)
		if(0 < u32snwidth(&line->str[tail], 1))
			break;

	deleteChars(line, head, tail - head);

	for (i = 0; i < rpad + lpad; i++)
		insertU32s(line, head, &space, 1);

	return head + lpad;
}

int
putU32s(Line *line, int col, const char32_t *str, int attr, Color fg, Color bg, size_t len)
{
	const int width = u32snwidth(str, len);
	int attrs[len];
	Color fgs[len], bgs[len];
	int head;
	InsertLine placed;
	int i;

	if (col < 0)
		return 0;

	for (i = 0; i < len; i++) {
		attrs[i] = attr;
		fgs[i]   = fg;
		bgs[i]   = bg;
	}
	placed = (InsertLine){ str, attrs, fgs, bgs };

	head = eraseInLine(line, col, width);
	insertU32s(line, head, &placed, len);

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

size_t
u32slen(const char32_t *str)
{
	return wcslen((const wchar_t *)str);
}

int
u32snwidth(const char32_t *str, int len)
{
	int width, total;
	int i;

	for (i = total = 0; i < len && str[i] != L'\0'; i++) {
		width = wcwidth(str[i]);
		total += width < 0 ? 2 : width;
	}

	return total;
}

CharCnt
getCharCnt(const char32_t *str, int col)
{
	const int len = u32slen(str);
	int width, total;
	int i;

	if (col < 0)
		return (CharCnt){ col, col, 1 };

	for (i = 0, total = 0; i < len; i++) {
		width = wcwidth(str[i]);
		width = width < 0 ? 2 : width;
		if (col < total + width)
			return (CharCnt){ i, total, width };
		total += width;
	}

	return (CharCnt){ len + (col - total), col, 1 };
}

int
getIndex(const char32_t *str, int col)
{
	return getCharCnt(str, col).index;
}
