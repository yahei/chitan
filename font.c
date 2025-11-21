#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "font.h"
#include "util.h"

/*
 * XFont
 *
 * フォントやグリフのフォールバック先を管理する
 */

XftFontSuite *getFontSuiteGlyphs(XFont *, char32_t);
XftFontSuite *getFontSuiteFonts(XFont *, const char *);
char *getFontName(const unsigned char *, char32_t, char *, int);

XFont *
openFont(Display *disp, const char *pattern)
{
	XFont *xfont = xmalloc(sizeof(XFont));
	const char *opt_head;
	XGlyphInfo ginfo;

	*xfont = (XFont){ .disp = disp, .cw = 1, .ch = 1 };

	/* patternを:の前と:以降に分けて持つ */
	opt_head = strchr(pattern, ':');
	opt_head = opt_head ? opt_head : strchr(pattern, '\0');
	xfont->family = xmalloc(opt_head - pattern + 1);
	xfont->option = xmalloc(strlen(opt_head) + 1);
	snprintf((char *)xfont->family, opt_head - pattern + 1, "%s", pattern);
	snprintf((char *)xfont->option, strlen(opt_head) + 1, "%s", opt_head);

	/* メインフォントのロード */
	getFontSuiteFonts(xfont, pattern);
	if (!(*xfont->fonts[0])[FONT_NONE]) {
		closeFont(xfont);
		return NULL;
	}

	/* 文字の高さや横幅を取得 */
	xfont->ch = (*xfont->fonts[0])[FONT_NONE]->height;
	XftTextExtents32(disp, (*xfont->fonts[0])[FONT_NONE], (char32_t *)L"x", 1, &ginfo);
	xfont->cw = 0 < ginfo.width ? ginfo.width : xfont->ch / 2;
	xfont->ascent = (*xfont->fonts[0])[FONT_NONE]->ascent;

	return xfont;
}

void
closeFont(XFont *xfont)
{
	int i, j;

	for (i = 0; i < xfont->fonts_len; i++) {
		for (j = 0; j < 4; j++)
			if ((*xfont->fonts[i])[j])
				XftFontClose(xfont->disp, (*xfont->fonts[i])[j]);
		free(xfont->fonts[i]);
	}

	free(xfont->family);
	free(xfont->option);
	free(xfont->glyphs);
	free(xfont->fonts);

	free(xfont);
}

void
drawXFontString(XftDraw *draw, XftColor *color, XFont *xfont, int attr, int x, int y, int w, const FcChar32 *str, int num)
{
	XRectangle rect = { 0, -xfont->ascent, w, xfont->ch};
	XftFontSuite *font;
	int i;

	XftDrawSetClipRectangles(draw, x, y, &rect, 1);
	for (i = 0; i < num; i++) {
		font = XftCharIndex(xfont->disp, (*xfont->fonts[0])[attr], str[i]) ?
			xfont->fonts[0] : getFontSuiteGlyphs(xfont, str[i]);
		if ((*font)[attr])
			XftDrawString32(draw, color, (*font)[attr], x, y, &str[i], 1);
		x += xfont->cw * wcwidth(str[i]);
	}
}

XftFontSuite *
getFontSuiteGlyphs(XFont *xfont, char32_t codepoint)
{
	XftFontSuite *font;
	char fontname[256], pattern[256];
	int j;

	/* グリフリストにあればそのフォントを使う */
	for (j = 0; j < xfont->glyphs_len; j++)
		if (xfont->glyphs[j].codepoint == codepoint)
			return xfont->glyphs[j].font;

	/* グリフをリストに追加 */
	getFontName(xfont->family, codepoint, fontname, sizeof(fontname));
	snprintf(pattern, sizeof(pattern), "%s%s", fontname, xfont->option);
	font = getFontSuiteFonts(xfont, pattern);
	struct FallbackGlyph fbg = { codepoint, font };
	PUSH_BACK(xfont->glyphs, xfont->glyphs_len, fbg);

	return font;
}

XftFontSuite *
getFontSuiteFonts(XFont *xfont, const char *pattern)
{
	XftFontSuite *font;
	unsigned char *xftname;
	char name[strlen(pattern) + 64];
	int i;

	/* フォントリストにあればそれを使う */
	for (i = 0; i < xfont->fonts_len; i++) {
		FcPatternGetString((*xfont->fonts[i])[0]->pattern, FC_FAMILY, 0, &xftname);
		if (strncmp((char *)xftname, pattern, strlen((char *)xftname)) == 0)
			return (xfont->fonts[i]);
	}

	/* フォントをロードしてリストに追加 */
	font = xmalloc(sizeof(XftFontSuite));
	for (i = 0; i < 4; i++) {
		strcpy(name, pattern);
		strcat(name, i & FONT_BOLD   ? ":bold"   : "");
		strcat(name, i & FONT_ITALIC ? ":italic" : "");
		(*font)[i] = XftFontOpenName(xfont->disp, XDefaultScreen(xfont->disp), name);
	}
	PUSH_BACK(xfont->fonts, xfont->fonts_len, font);

	return font;
}

char *
getFontName(const unsigned char *pattern, char32_t codepoint, char *buf, int buflen)
{
	FcPattern *fcmatched, *fcpattern = FcPatternCreate();
	FcCharSet *fccharset = FcCharSetCreate();
	FcResult fcresult;
	unsigned char *family;

	FcPatternAddString(fcpattern, FC_FAMILY, pattern);
	FcCharSetAddChar(fccharset, codepoint);
	FcPatternAddCharSet(fcpattern, FC_CHARSET, fccharset);
	FcConfigSubstitute(NULL, fcpattern, FcMatchPattern);
	FcDefaultSubstitute(fcpattern);
	fcmatched = FcFontMatch(NULL, fcpattern, &fcresult);
	FcPatternGetString(fcmatched, FC_FAMILY, 0, &family);

	snprintf(buf, buflen, "%s", family);

	FcCharSetDestroy(fccharset);
	FcPatternDestroy(fcpattern);
	FcPatternDestroy(fcmatched);

	return buf;
}
