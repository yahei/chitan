#include <stdlib.h>
#include <string.h>

#include "line.h"
#include "util.h"

/*
 * Line
 *
 * バッファ一行分の情報を管理する
 */

struct Line {
	char *str;      /* UTF8文字列 */
	int len;        /* 文字数 */
};

Line *
newLine(void)
{
	Line *line = xmalloc(sizeof(Line));
	line->str = NULL;
	line->len = 0;
	setUtf8(line, "", 0);
	return line;
}

void
deleteLine(Line *line)
{
	if (line == NULL)
		return;

	free(line->str);
	line->str = NULL;

	free(line);
}

void
setUtf8(Line *line, char *str, int size)
{
	line->str = xrealloc(line->str, size + 1);
	strncpy(line->str, str, size);
	line->str[size] = '\0';
}

const char *
getUtf8(Line *line)
{
	return line->str;
}

void
overwriteUtf8(Line *line, char *str, int size, int pos)
{
	int newlen = MAX(pos + size, line->len);

	/* 必要なら文字列を伸ばす */
	if (line->len < newlen) {
		line->str = xrealloc(line->str, newlen + 1);
		line->str[newlen] = '\0';
		line->len = newlen;
	}

	/* 文字列を書き込む */
	strncpy(&line->str[pos], str, size);
}
