#include <X11/Xft/Xft.h>

typedef uint_least32_t char32_t;
typedef XftFont *(XftFontSuite[8]);

enum font_attributes {
	FONT_NONE    = 0,
	FONT_BOLD    = 1 << 0,   /* 太字 */
	FONT_FAINT   = 1 << 1,   /* 細字 */
	FONT_ITALIC  = 1 << 2,   /* 斜体 */
};

typedef struct XFont {
	Display *disp;
	unsigned char *pattern;
	int cw, ch;
	int ascent;
	struct FallbackGlyph {
		char32_t codepoint;
		XftFontSuite *font;
	} *glyphs;
	XftFontSuite **fonts;
	int glyphs_len, fonts_len;
} XFont;

XFont *openFont(Display *, const char *);
void closeFont(XFont *);
void drawXFontString(XftDraw *, XftColor *, XFont *, int, int, int, const FcChar32 *, int);
