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

/*
 * 文字列を挿入する関数
 *
 * 末尾より後に挿入すると、末尾から挿入箇所までがスペースで埋められる
 * lenより短いNULL終端文字列を渡した場合の動作は未定義
 */
void
insertU8(Line *line, int head, const char *str, int len)
{
	int i;
	const int oldlen = u32slen(line->str);
	const int newlen = oldlen + len + MAX(head - oldlen, 0) + 1;
	const int movelen = MAX(oldlen - head, 0) + 1;

	/* 文字列を伸ばして書き込む場所を作る */
	line->str = xrealloc(line->str, newlen * sizeof(char32_t));
	memmove(&line->str[head + len], &line->str[MIN(head, oldlen)],
			movelen * sizeof(char32_t));
	for (i = oldlen; i < head; i++)
		line->str[i] = L' ';

	/* 挿入する文字列を書き込む */
	u8sToU32s(str, &line->str[head], len);

	/* 末尾にスペースがあれば取り除く */
	for (i = u32slen(line->str); 0 < i; i--)
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
	int oldlen = u32slen(line->str);
	int tail = head + len;

	/* 範囲チェック */
	head = MAX(head, 0);
	tail = MIN(tail, oldlen);
	len = tail - head;
	if (len <= 0)
		return;

	/* 削除後の文字列を作る */
	char32_t *newstr = xmalloc((oldlen - len + 1) * sizeof(char32_t));
	memcpy(newstr, line->str, head * sizeof(char32_t));
	memcpy(&newstr[head], &line->str[tail],
			(oldlen - tail + 1) * sizeof(char32_t));

	/* lineの文字列を削除後のものに置き換える */
	free(line->str);
	line->str = newstr;

	/* 末尾にスペースがあれば取り除く */
	for (i = u32slen(line->str); 0 < i; i--)
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
putU8(Line *line, int pos, const char *str, int len)
{
	const int linelen = u32slen(line->str);
	int head, tail; /* 消去する範囲 */
	int lpad, rpad; /* 幅広文字を消した後のスペースの数 */
	int width;      /* 置く文字列の表示幅 */

	if (pos < 0)
		return;

	/* 置く文字列の表示幅を調べる */
	char32_t *buf = xmalloc(len * sizeof(char32_t));
	u8sToU32s(str, buf, len);
	width = u32swidth(buf, len);
	free(buf);

	/* headとlpad */
	for (head = 0; head < linelen; head++)
		if (pos < u32swidth(line->str, head + 1))
			break;
	lpad = pos - u32swidth(line->str, head);

	/* tailとrpad */
	for (tail = head; tail < linelen; tail++)
		if (pos + width <= u32swidth(line->str, tail))
			break;
	rpad = u32swidth(line->str, tail) - (pos + width);

	/* tailの後が結合文字なら削除範囲に含める */
	for (; tail < linelen; tail++)
		if(0 < u32swidth(&line->str[tail], 1))
			break;

	/* 幅広文字を消した後のスペースを置く */
	head += lpad;
	tail += lpad;
	for (; rpad > 0; rpad--)
		insertU8(line, tail - lpad, " ", 1);
	for (; lpad > 0; lpad--)
		insertU8(line, head - lpad, " ", 1);

	/* 削除と挿入を行う */
	deleteChars(line, head, tail - head);
	insertU8(line, head, str, len);
}

/*
 * マルチバイト文字列を操作する関数
 */

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
	int i, width;

	for (i = width = 0; i < len && str[i] != L'\0'; i++)
		width += MAX(wcwidth(str[i]), 0);

	return width;
}

/*
 * 表示位置を指定してそこが何文字目か調べる
 * posとして負の数を渡した場合の動作は未定義
 */
int
u32sposlen(const char32_t *str, int pos)
{
	const int linelen = u32slen(str);
	int len;

	for (len = 0; len < linelen; len++)
		if (pos < u32swidth(str, len + 1))
			return len;

	return linelen + pos - u32swidth(str, linelen);
}
