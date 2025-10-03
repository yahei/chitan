#include <stdint.h>
#include <string.h>
#include <wchar.h>

#include "font.h"
#include "util.h"

XftFontSuite *getFontSuiteGlyphs(XFont *, char32_t);
XftFontSuite *getFontSuiteFonts(XFont *, const char *);
char *getFontName(const unsigned char *, char32_t, char *, int);

XFont *
openFont(Display *disp, const char *family, float size)
{
	XFont *xfont = xmalloc(sizeof(XFont));
	XGlyphInfo ginfo;

	*xfont = (XFont){ disp, NULL, size, 1, 1, 0, NULL, NULL, 0, 0 };
	xfont->family = xmalloc(strlen(family) + 1);
	strcpy((char *)xfont->family, family);

	/* メインフォントのロード */
	getFontSuiteFonts(xfont, family);
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
		for (j = 0; j < 8; j++)
			if ((*xfont->fonts[i])[j])
				XftFontClose(xfont->disp, (*xfont->fonts[i])[j]);
		free(xfont->fonts[i]);
	}

	free(xfont->family);
	free(xfont->glyphs);
	free(xfont->fonts);

	free(xfont);
}

void
drawXFontString(XftDraw *draw, XftColor *color, XFont *xfont, int attr, int x, int y, const FcChar32 *str, int num)
{
	XftFontSuite *font;
	int i;

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
	char fontname[256];
	int j;

	/* グリフリストにあればそのフォントを使う */
	for (j = 0; j < xfont->glyphs_len; j++)
		if (xfont->glyphs[j].codepoint == codepoint)
			return xfont->glyphs[j].font;

	/* グリフをリストに追加 */
	getFontName(xfont->family, codepoint, fontname, 256);
	font = getFontSuiteFonts(xfont, fontname);
	struct FallbackGlyph fbg = { codepoint, font };
	PUSH_BACK(xfont->glyphs, xfont->glyphs_len, fbg);

	return font;
}

XftFontSuite *
getFontSuiteFonts(XFont *xfont, const char *fontname)
{
	XftFontSuite *font;
	unsigned char *xftname;
	int i, j, weight, slant;

	/* フォントリストにあればそれを使う */
	for (j = 0; j < xfont->fonts_len; j++) {
		FcPatternGetString((*xfont->fonts[j])[0]->pattern, FC_FAMILY, 0, &xftname);
		if (strcmp((char *)xftname, fontname) == 0)
			return (xfont->fonts[j]);
	}

	/* フォントをロードしてリストに追加 */
	font = xmalloc(sizeof(XftFontSuite));
	for (i = 0; i < 8; i++) {
		weight = i & FONT_BOLD ? XFT_WEIGHT_BOLD : XFT_WEIGHT_MEDIUM;
		slant = i & FONT_ITALIC ? XFT_SLANT_ITALIC : XFT_SLANT_ROMAN;
		(*font)[i] = XftFontOpen(
				xfont->disp, 0,
				XFT_FAMILY, XftTypeString, fontname,
				XFT_SIZE, XftTypeDouble, xfont->size,
				XFT_WEIGHT, XftTypeInteger, weight,
				XFT_SLANT, XftTypeInteger, slant,
				NULL);
	}
	PUSH_BACK(xfont->fonts, xfont->fonts_len, font);

	return font;
}

char *
getFontName(const unsigned char *family, char32_t codepoint, char *buf, int buflen)
{
	FcPattern *matched, *pattern = FcPatternCreate();
	FcCharSet *charset = FcCharSetCreate();
	FcResult result;
	unsigned char *fontname;

	FcPatternAddString(pattern, FC_FAMILY, family);
	FcCharSetAddChar(charset, codepoint);
	FcPatternAddCharSet(pattern, FC_CHARSET, charset);
	FcConfigSubstitute(NULL, pattern, FcMatchPattern);
	FcDefaultSubstitute(pattern);
	matched = FcFontMatch(NULL, pattern, &result);
	FcPatternGetString(matched, FC_FAMILY, 0, &fontname);

	strncpy(buf, (char *)fontname, buflen);
	buf[buflen-1] = '\0';

	FcCharSetDestroy(charset);
	FcPatternDestroy(pattern);
	FcPatternDestroy(matched);

	return buf;
}
