#define _XOPEN_SOURCE 600

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <fontconfig/fontconfig.h>

#include "line.h"
#include "util.h"

/*
 * Line
 *
 * バッファ一行分の情報を管理する
 * 行末のスペースは自動的に取り除かれる
 * 挿入と削除に加え、文字がある場所や末尾以降に文字列を置くこともできる
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
insertU8(Line *line, int head, const char *str, int len)
{
	const int oldlen = u32slen(line->str);
	const int newlen = oldlen + len + MAX(head - oldlen, 0) + 1;
	const int movelen = MAX(oldlen - head, 0) + 1;
	int i;

	line->str = xrealloc(line->str, newlen * sizeof(char32_t));
	memmove(&line->str[head + len], &line->str[MIN(head, oldlen)],
			movelen * sizeof(char32_t));
	for (i = oldlen; i < head; i++)
		line->str[i] = L' ';

	u8sToU32s(str, &line->str[head], len);

	for (i = u32slen(line->str); 0 < i; i--)
		if (line->str[i - 1] != L' ')
			break;
	line->str[i] = L'\0';
}

void
deleteChars(Line *line, int head, int len)
{
	const int oldlen = u32slen(line->str);
	char32_t *newstr;
	int tail = head + len;
	int i;

	head = MAX(head, 0);
	tail = MIN(tail, oldlen);
	len = tail - head;
	if (len <= 0)
		return;

	newstr = xmalloc((oldlen - len + 1) * sizeof(char32_t));
	memcpy(newstr, line->str, head * sizeof(char32_t));
	memcpy(&newstr[head], &line->str[tail],
			(oldlen - tail + 1) * sizeof(char32_t));

	free(line->str);
	line->str = newstr;

	for (i = u32slen(line->str); 0 < i; i--)
		if (line->str[i - 1] != L' ')
			break;
	line->str[i] = L'\0';
}

int
putU8(Line *line, int col, const char *str, int len)
{
	const int linelen = u32slen(line->str);
	char32_t buf[len * sizeof(char32_t)];
	int width;
	int head, tail;
	int lpad, rpad;

	if (col < 0)
		return 0;

	u8sToU32s(str, buf, len);
	width = u32swidth(buf, len);

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
	for (; rpad > 0; rpad--)
		insertU8(line, tail - lpad, " ", 1);
	for (; lpad > 0; lpad--)
		insertU8(line, head - lpad, " ", 1);

	deleteChars(line, head, tail - head);
	insertU8(line, head, str, len);

	return width;
}

void
u8sToU32s(const char *src, char32_t *dest, int len)
{
	int i;

	for (i = 0; i < len; i++)
		src += FcUtf8ToUcs4((const FcChar8 *)src, dest++, 4);
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

	for (len = 0; len < linelen; len++)
		if (col < u32swidth(str, len + 1))
			return len;

	return linelen + col - u32swidth(str, linelen);
}
