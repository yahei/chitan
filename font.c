#include <stdint.h>
#include <wchar.h>

#include "font.h"
#include "util.h"

typedef uint_least32_t char32_t;

XFont *
openFont(Display *disp, const char *name, float size)
{
	XFont *xfont = xmalloc(sizeof(XFont));
	XGlyphInfo ginfo;
	int i, weight, slant;

	xfont->disp = disp;

	for (i = 0; i < 8; i++) {
		weight = i & FONT_BOLD ? XFT_WEIGHT_BOLD : XFT_WEIGHT_MEDIUM;
		slant = i & FONT_ITALIC ? XFT_SLANT_ITALIC : XFT_SLANT_ROMAN;
		xfont->fonts[i] = XftFontOpen(
				disp, 0,
				XFT_FAMILY, XftTypeString, name,
				XFT_SIZE, XftTypeDouble, size,
				XFT_WEIGHT, XftTypeInteger, weight,
				XFT_SLANT, XftTypeInteger, slant,
				NULL);
	}

	for (i = 0; i < 8; i++) {
		if (xfont->fonts[i] == NULL) {
			closeFont(xfont);
			return NULL;
		}
	}

	/* テキストの高さや横幅を取得 */
	xfont->ch = xfont->fonts[FONT_NONE]->height - 1.0;
	XftTextExtents32(disp, xfont->fonts[FONT_NONE], (char32_t *)L"x", 1, &ginfo);
	xfont->cw = ginfo.width;

	return xfont;
}

void
closeFont(XFont *xfont) {
	int i;

	for (i = 0; i < 8; i++)
		if (xfont->fonts[i])
			XftFontClose(xfont->disp, xfont->fonts[i]);
	free(xfont);
}

void
drawXFontString(XftDraw *draw, XftColor *color, XFont *xfont, int attr, int x, int y, const FcChar32 *str, int num)
{
	int i;
	XftFont *font = xfont->fonts[attr];

	for (i = 0; i < num; i++) {
		XftDrawString32(draw, color, font, x, y, &str[i], 1);
		x += xfont->cw * wcwidth(str[i]);
	}
}
