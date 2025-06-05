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
 */

Line *
allocLine(void)
{
	Line *line = xmalloc(sizeof(Line));

	line->str = xmalloc(sizeof(utf32));
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

/*
 * 文字列を挿入する関数
 *
 * 末尾より後に挿入すると、末尾から挿入箇所までがスペースで埋められる
 * lenより短いNULL終端文字列を渡した場合の動作は未定義
 */
void
insertUtf8(Line *line, int head, const char *str, int len)
{
	int i;
	const int oldlen = utf32slen(line->str);
	const int newlen = oldlen + len + MAX(head - oldlen, 0) + 1;
	const int movelen = MAX(oldlen - head, 0) + 1;

	/* 文字列を伸ばして書き込む場所を作る */
	line->str = xrealloc(line->str, newlen * sizeof(utf32));
	memmove(&line->str[head + len], &line->str[MIN(head, oldlen)],
			movelen * sizeof(utf32));
	for (i = oldlen; i < head; i++)
		line->str[i] = L' ';

	/* 挿入する文字列を書き込む */
	utf8sToUtf32s(str, &line->str[head], len);

	/* 末尾にスペースがあれば取り除く */
	for (i = utf32slen(line->str); 0 < i; i--)
		if (line->str[i - 1] != L' ')
			break;
	line->str[i] = L'\0';
}

/*
 * 文字列の一部を削除する関数
 */
void
deleteChars(Line *line, int head, int len)
{
	int i;
	int oldlen = utf32slen(line->str);
	int tail = head + len;

	/* 範囲チェック */
	head = MAX(head, 0);
	tail = MIN(tail, oldlen);
	len = tail - head;
	if (len <= 0)
		return;

	/* 削除後の文字列を作る */
	utf32 *newstr = xmalloc((oldlen - len + 1) * sizeof(utf32));
	memcpy(newstr, line->str, head * sizeof(utf32));
	memcpy(&newstr[head], &line->str[tail],
			(oldlen - tail + 1) * sizeof(utf32));

	/* lineの文字列を削除後のものに置き換える */
	free(line->str);
	line->str = newstr;

	/* 末尾にスペースがあれば取り除く */
	for (i = utf32slen(line->str); 0 < i; i--)
		if (line->str[i - 1] != L' ')
			break;
	line->str[i] = L'\0';
}

/*
 * 指定した位置に文字列を置く関数
 *
 * 指定された場所に既に文字がある場合、上書きして文字を置く
 * 置く文字が全角文字に半分だけ重なった場合、もう半分は半角スペースになる
 * 末尾より後に置く場合、隙間は半角スペースで埋められる
 * lenより短いNULL終端文字列を渡した場合の動作は未定義
 * posとして負の値を渡した場合の動作は未定義
 */
void
putUtf8(Line *line, int pos, const char *str, int len)
{
	const int linelen = utf32slen(line->str);
	int i;
	int head, tail; // 消去する範囲
	int lpad, rpad; // 幅広文字を消した後のスペースの数
	int width;      // 置く文字列の表示幅

	if (pos < 0)
		return;

	/* 置く文字列の表示幅を調べる */
	utf32 *buf = xmalloc(len * sizeof(utf32));
	utf8sToUtf32s(str, buf, len);
	width = utf32swidth(buf, len);
	free(buf);

	/* headとlpad */
	for (i = 0; i < linelen + 1; i++)
		if (pos < utf32swidth(line->str, i))
			break;
	head = i - 1;
	lpad = pos - utf32swidth(line->str, head);

	/* tailとrpad */
	for (i = head; i < linelen; i++)
		if (pos + width <= utf32swidth(line->str, i))
			break;
	tail = i;
	rpad = utf32swidth(line->str, tail) - (pos + width);

	/* 幅広文字を消した後のスペースを置く */
	for (i = rpad; i > 0; i--)
		insertUtf8(line, tail, " ", 1);
	for (i = lpad; i > 0; i--)
		insertUtf8(line, head, " ", 1);
	head += lpad;
	tail += lpad;

	/* 削除と挿入を行う */
	deleteChars(line, head, tail - head);
	insertUtf8(line, head, str, len);
}

/*
 * マルチバイト文字列を操作する関数
 */

void
utf8sToUtf32s(const char *src, utf32 *dest, int len)
{
	int i;

	for (i = 0; i < len; i++)
		src += FcUtf8ToUcs4((const FcChar8 *)src, dest++, 4);
}

int
utf32slen(const utf32 *str)
{
	return wcslen((const wchar_t *)str);
}

int
utf32swidth(const utf32 *str, size_t len)
{
	int i, width;

	for (i = width = 0; i < len && str[i] != L'\0'; i++)
		width += MAX(wcwidth(str[i]), 0);

	return width;
}
