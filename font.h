#include <X11/Xft/Xft.h>

enum font_attributes {
	FONT_NONE    = 0,
	FONT_BOLD    = 1 << 0,   /* 太字 */
	FONT_FAINT   = 1 << 1,   /* 細字 */
	FONT_ITALIC  = 1 << 2,   /* 斜体 */
};

typedef struct XFont {
	Display *disp;
	XftFont *fonts[8];
	int cw, ch;
} XFont;

XFont *openFont(Display *, const char *, float);
void closeFont(XFont *);
void drawXFontString(XftDraw *, XftColor *, XFont *, int, int, int, const FcChar32 *, int);
