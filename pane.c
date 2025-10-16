#include <wchar.h>

#include "pane.h"
#include "util.h"

#define CHOOSE(a, b, c) (timespeccmp((a), (b), c) ? (a) : (b))
#define BLEND_COLOR(c1, a1, c2, a2) (\
		((int)(ALPHA(c1) * (a1) + ALPHA(c2) * (a2)) << 24) +\
		((int)(  RED(c1) * (a1) +   RED(c2) * (a2)) << 16) +\
		((int)(GREEN(c1) * (a1) + GREEN(c2) * (a2)) <<  8) +\
		((int)( BLUE(c1) * (a1) +  BLUE(c2) * (a2)) <<  0))
#define BELLCOLOR(color) (pane->timer_lit[BELL_TIMER] ?\
		BLEND_COLOR(color, 0.85, 0xffffffff, 0.15) : color)
#define SCROLLMAX(sb)   (sb->firstline - MAX(sb->totallines - sb->maxlines, 0))

Pane *
createPane(Display *disp, XFont *xfont, Drawable d, int width, int height, int depth)
{
	XVisualInfo vinfo;
	XMatchVisualInfo(disp, XDefaultScreen(disp), 32, TrueColor, &vinfo);
	Visual *visual = vinfo.visual;
	Colormap cmap = XCreateColormap(disp,
			DefaultRootWindow(disp), visual, None);

	Pane *pane = xmalloc(sizeof(Pane));

	*pane = (Pane){
		.disp = disp, .xfont = xfont, .d = d, .depth = depth,
		.width = width, .height = height,
		.xpad = xfont->cw / 2, .ypad = xfont->cw / 2
	};
	memset(&pane->timers, 0, TIMER_NUM * sizeof(struct timespec));
	memset(&pane->timer_active, 0, TIMER_NUM);
	memset(&pane->timer_lit, 0, TIMER_NUM);

	/* 端末をオープン */
	pane->term = openTerm((height - pane->ypad * 2) / xfont->ch,
			(width - pane->xpad * 2) / xfont->cw, 256);
	if (!pane->term)
		errExit("openTerm failed.\n");
	pane->term->bell = bell;
	pane->term->bellp = pane;
	pane->term->palette[defbg] = 0xcc000000 + (0x00ffffff & pane->term->palette[defbg]);

	/* 描画の準備 */
	pane->pixmap = XCreatePixmap(disp, d, width, height, depth);
	pane->gc = XCreateGC(disp, pane->pixmap, 0, NULL);
	pane->draw = XftDrawCreate(disp, pane->pixmap, visual, cmap);

	return pane;
}

void
destroyPane(Pane *pane)
{
	closeTerm(pane->term);
	XftDrawDestroy(pane->draw);
	XFreeGC(pane->disp, pane->gc);
	XFreePixmap(pane->disp, pane->pixmap);
	free(pane);
}

void
setPaneSize(Pane *pane, int width, int height)
{
	XVisualInfo vinfo;
	XMatchVisualInfo(pane->disp, XDefaultScreen(pane->disp), 32, TrueColor, &vinfo);
	Visual *visual = vinfo.visual;
	Colormap cmap = XCreateColormap(pane->disp,
			DefaultRootWindow(pane->disp), visual, None);

	XftDrawDestroy(pane->draw);
	XFreeGC(pane->disp, pane->gc);
	XFreePixmap(pane->disp, pane->pixmap);

	pane->pixmap = XCreatePixmap(pane->disp, pane->d, width, height, pane->depth);
	pane->gc = XCreateGC(pane->disp, pane->pixmap, 0, NULL);
	pane->draw = XftDrawCreate(pane->disp, pane->pixmap, visual, cmap);
	pane->width = width;
	pane->height = height;

	setWinSize(pane->term, (height - pane->ypad * 2) / pane->xfont->ch,
			(width - pane->xpad * 2) / pane->xfont->cw, width, height);
}

void
mouseEvent(Pane *pane, XEvent *event)
{
	int mb, state = event->xbutton.state;

	if (event->type == MotionNotify) {
		mb = (state & Button1Mask ?  0 :
		      state & Button2Mask ?  1 :
		      state & Button3Mask ?  2 : 3) + MOVE;
	} else {
		mb = event->xbutton.button;
		mb = BETWEEN(mb, 1, 4)  ? (mb - 1) :
		     BETWEEN(mb, 4, 8)  ? (mb - 4) + WHEEL :
		     BETWEEN(mb, 8, 12) ? (mb - 8) + OTHER : 3;
	}
	mb += state & ShiftMask   ? SHIFT : 0;
	mb += state & Mod1Mask    ? ALT   : 0;
	mb += state & ControlMask ? CTRL  : 0;
	reportMouse(pane->term, mb, event->type == ButtonRelease,
			(event->xbutton.x - pane->xpad) / pane->xfont->cw,
			(event->xbutton.y - pane->ypad) / pane->xfont->ch);
}

void
scrollPane(Pane *pane, int n)
{
	pane->redraw_flag = 1;
	pane->scr += n;
	pane->scr = CLIP(pane->scr, 0, SCROLLMAX(pane->term->sb));
}

void
copySelection(Pane *pane, char **dst)
{
	int len = 256;
	char32_t *cp, *copy = xmalloc(len * sizeof(copy[0]));
	int firstline = MIN(pane->selection.aline, pane->selection.bline);
	int lastline  = MAX(pane->selection.aline, pane->selection.bline);
	int left      = MIN(pane->selection.acol,  pane->selection.bcol);
	int right     = MAX(pane->selection.acol,  pane->selection.bcol);
	Line *line;
	int i, l, r;

	copy[0] = L'\0';

	/* 選択範囲の文字列(UTF32)を読み出してコピー */
	for(i = firstline; i <= lastline; i++) {
		if (!(line = getLine(pane->term, i - pane->term->sb->firstline)))
			continue;

		if (pane->selection.rect) {
			l = MIN(getCharCnt(line->str,  left).index, u32slen(line->str));
			r = MIN(getCharCnt(line->str, right).index, u32slen(line->str));
		} else {
			l = (i != firstline) ? 0 :
				MIN(getCharCnt(line->str, left).index, u32slen(line->str));
			r = (i != lastline) ? u32slen(line->str) + 1 :
				MIN(getCharCnt(line->str, right).index, u32slen(line->str));
		}

		while (len < u32slen(copy) + r - l + 2) {
			len += 256;
			copy = xrealloc(copy, len * sizeof(copy[0]));
		}
		cp = copy + u32slen(copy);
		wcsncpy((wchar_t *)cp, (wchar_t *)line->str + l, r - l);
		cp[r - l] = L'\0';
		if (i < lastline)
			wcscat((wchar_t *)copy, L"\n");
	}

	/* UTF8に変換して保存 */
	*dst = xrealloc(*dst, len * 4);
	wcstombs(*dst, (wchar_t *)copy, len * 4);

	free(copy);
}

void
manegeTimer(Pane *pane, struct timespec *timeout)
{
	struct timespec nexttime;
	static const struct timespec duration[] = {
		[BELL_TIMER]  = { 0, 150 * 1000 * 1000 },
		[BLINK_TIMER] = { 0, 800 * 1000 * 1000 },
		[RAPID_TIMER] = { 0, 200 * 1000 * 1000 },
		[CARET_TIMER] = { 0, 500 * 1000 * 1000 },
	};
	int i;

	/* 点滅させる必要がないときはキャレットのタイマーを止める */
	pane->timer_active[CARET_TIMER] = pane->focus &&
		(pane->term->cy + pane->scr <= pane->term->sb->rows) &&
		(!pane->term->ctype || pane->term->ctype % 2);

	/* タイムアウトの設定 */
	nexttime = (struct timespec){ 1 << 16, 0 };
	timespecadd(&pane->now, &nexttime, &nexttime);
	for (i = 0; i < TIMER_NUM; i++) {
		if (!pane->timer_active[i])
			continue;
		if (timespeccmp(&pane->timers[i], &pane->now, <=)) {
			pane->timer_lit[i] = !pane->timer_lit[i];
			pane->redraw_flag = 1;
			timespecadd(&pane->timers[i], &duration[i], &pane->timers[i]);
			if (timespeccmp(&pane->timers[i], &pane->now, <=))
				timespecadd(&pane->now, &duration[i], &pane->timers[i]);
		}
		nexttime = *CHOOSE(CHOOSE(&pane->timers[i], &nexttime, <), &pane->now, >);
	}
	timespecsub(&nexttime, &pane->now, timeout);

	/* ベルは繰り返さない */
	if (pane->timer_lit[BELL_TIMER] == 0)
		pane->timer_active[BELL_TIMER] = 0;
}

void
drawPane(Pane *pane, Line *peline, int pecaret)
{
	Line *line;
	int pepos, pewidth, pecaretpos, caretrow;
	int i;

	/* スクロール量の更新 */
	pane->scr = (pane->prevbuf != pane->term->sb) ? 0 : pane->scr;
	pane->scr += (0 < pane->scr) ? pane->term->sb->firstline - pane->prevfst : 0;
	pane->scr = CLIP(pane->scr, 0, SCROLLMAX(pane->term->sb));
	pane->prevbuf = pane->term->sb;
	pane->prevfst = pane->term->sb->firstline;

	/* 点滅中フラグをクリア */
	pane->timer_active[BLINK_TIMER] = pane->timer_active[RAPID_TIMER] = 0;

	/* 画面をクリア */
	XSetForeground(pane->disp, pane->gc, BELLCOLOR(pane->term->palette[defbg]));
	XFillRectangle(pane->disp, pane->pixmap, pane->gc, 0, 0, pane->width, pane->height);

	/* 端末の内容をウィンドウに表示 */
	for (i = 0; i <= pane->term->sb->rows; i++)
		if ((line = getLine(pane->term, i - pane->scr)))
			drawLine(pane, line, i, 0, pane->term->sb->cols, 0);

	/* カーソルかPreeditを表示 */
	XSetForeground(pane->disp, pane->gc, pane->term->palette[deffg]);
	if (u32slen(peline->str)) {
		/* Preeditの幅とキャレットのPreedit内での位置を取得 */
		pewidth = u32swidth(peline->str, u32slen(peline->str));
		pecaretpos = u32swidth(peline->str, pecaret);

		/* Preeditの画面上での描画位置を決める */
		pepos = pane->term->cx - pecaretpos;
		pepos = MIN(pepos, 0);
		pepos = MAX(pepos, pane->term->sb->cols - pewidth);
		pepos = MIN(pepos, pane->term->cx);

		/* Preeditとカーソルの描画 */
		drawLine(pane, peline, pane->term->cy, pepos, pewidth, 0);
		drawCursor(pane, peline, pane->term->cy, pepos + pecaretpos, 6);
	} else if (1 <= pane->term->dec[25] && pane->term->cx < pane->term->sb->cols) {
		/* カーソルの描画 */
		caretrow = pane->term->cy + pane->scr;
		if (caretrow <= pane->term->sb->rows && (line = getLine(pane->term, pane->term->cy)))
			drawCursor(pane, line, caretrow, pane->term->cx, pane->term->ctype);
	}

	/* 選択範囲を書く */
	if ((pane->selection.aline != pane->selection.bline ||
	     pane->selection.acol  != pane->selection.bcol) &&
	    pane->selection.altbuf == (pane->term->sb == &pane->term->alt))
		drawSelection(pane, &pane->selection);

	pane->redraw_flag = 0;
}

void
drawLine(Pane *pane, Line *line, int row, int col, int width, int pos)
{
	int next, i = getCharCnt(line->str, pos).index;
	int x, y, w;
	int attr, fg, bg;
	XftColor xc;
	Color c;

	if (width <= pos || line->str[i] == L'\0')
		return;

	/* 同じ属性の文字はまとめて処理する */
	next = MIN(findNextSGR(line, i), width);
	drawLine(pane, line, row, col, width, pos + u32swidth(&line->str[i], next - i));

	/* 点滅 */
	if (line->attr[i] & BLINK)
		pane->timer_active[BLINK_TIMER] = 1;
	if (line->attr[i] & RAPID)
		pane->timer_active[RAPID_TIMER] = 1;
	if (!(((line->attr[i] & BLINK) && pane->timer_lit[BLINK_TIMER]) ||
	      ((line->attr[i] & RAPID) && pane->timer_lit[RAPID_TIMER]) ||
	      (!(line->attr[i] & BLINK) && !(line->attr[i] & RAPID))))
		return;

	/* 座標 */
	x = pane->xpad + (col + pos) * pane->xfont->cw;
	y = pane->ypad + row * pane->xfont->ch;
	w = pane->xfont->cw * u32swidth(&line->str[i], next - i);

	/* 前処理 */
	if (line->attr[i] & NEGA) { /* 反転 */
		fg = line->bg[i];
		bg = line->fg[i];
	} else {
		fg = line->fg[i];
		bg = line->bg[i];
	}
	if (line->attr[i] & BOLD) /* 太字 */
		fg = fg < 8 ? fg + 8 : fg;
	c = pane->term->palette[fg]; /* 色を取得 */
	if (line->attr[i] & FAINT) /* 細字 */
		c = BLEND_COLOR(c, 0.6, pane->term->palette[bg], 0.4);

	/* 色をXftColorに変換 */
	xc.color.red   =   RED(c) << 8;
	xc.color.green = GREEN(c) << 8;
	xc.color.blue  =  BLUE(c) << 8;
	xc.color.alpha = 0xffff;

	/* 背景を塗る */
	XSetForeground(pane->disp, pane->gc, BELLCOLOR(pane->term->palette[bg]));
	XFillRectangle(pane->disp, pane->pixmap, pane->gc, x, y, w, pane->xfont->ch);

	y += pane->xfont->ascent;

	/* 文字を書く */
	attr = FONT_NONE;
	attr |= line->attr[i] & BOLD   ? FONT_BOLD   : FONT_NONE;
	attr |= line->attr[i] & ITALIC ? FONT_ITALIC : FONT_NONE;
	drawXFontString(pane->draw, &xc, pane->xfont, attr, x, y, &line->str[i], next - i);

	/* 後処理 */
	if (line->attr[i] & ULINE) { /* 下線 */
		XSetForeground(pane->disp, pane->gc, pane->term->palette[fg]);
		XDrawLine(pane->disp, pane->pixmap, pane->gc, x, y + 1, x + w - 1, y + 1);
	}
}

void
drawCursor(Pane *pane, Line *line, int row, int col, int type)
{
	const int index = getCharCnt(line->str, col).index;
	char32_t *c = index < u32slen(line->str) ? &line->str[index] : (char32_t *)L" ";
	const int x = pane->xpad + col * pane->xfont->cw;
	const int y = pane->ypad + row * pane->xfont->ch;
	const int width = pane->xfont->cw * u32swidth(c, 1) - 1;
	int attr;
	Line cursor;

	/* 点滅 */
	if (pane->focus && (!type || type % 2) && (pane->timer_lit[CARET_TIMER]))
		return;

	switch (type) {
	default: /* ブロック */
	case 0:
	case 1:
	case 2:
		if (pane->focus) {
			attr = index < u32slen(line->str) ? line->attr[index] : 0;
			cursor = (Line){c, &attr, &defbg, &deffg};
			drawLine(pane, &cursor, row, col, 1, 0);
		} else {
			XSetForeground(pane->disp, pane->gc, BELLCOLOR(pane->term->palette[deffg]));
			XDrawRectangle(pane->disp, pane->pixmap, pane->gc, x, y, width, pane->xfont->ch - 1);
			XDrawPoint(pane->disp, pane->pixmap, pane->gc, x + width, y + pane->xfont->ch - 1);
		}
		break;
	case 3: /* 下線 */
	case 4:
		XFillRectangle(pane->disp, pane->pixmap, pane->gc, x, y + 1 + pane->xfont->ascent, width, pane->xfont->ch * 0.1);
		break;
	case 5: /* 縦線 */
	case 6:
		XFillRectangle(pane->disp, pane->pixmap, pane->gc, x - 1, y, pane->xfont->ch * 0.1, pane->xfont->ch);
		break;
	}
}

void
drawSelection(Pane *pane, struct Selection *sel)
{
	const int s = MIN(sel->aline, sel->bline) - pane->term->sb->firstline;
	const int e = MAX(sel->aline, sel->bline) - pane->term->sb->firstline;
	int i;

#define DRAW(n, a, b)   drawLineRev(pane, getLine(pane->term, n), n + pane->scr, a, b)
	if (sel->rect) {
		/* 矩形選択 */
		for (i = s; i < e + 1; i++)
			DRAW(i, sel->acol, sel->bcol);
	} else {
		/* 通常 */
		if (sel->aline == sel->bline) {
			DRAW(s, sel->acol, sel->bcol);
		} else {
			DRAW(s, (sel->aline < sel->bline ? sel->acol : sel->bcol), pane->term->sb->cols + 1);
			DRAW(e, 0, (sel->aline < sel->bline ? sel->bcol : sel->acol));
			for (i = s + 1; i < e; i++)
				DRAW(i, 0, pane->term->sb->cols + 1);
		}
	}
#undef DRAW
}

void
drawLineRev(Pane *pane, Line *line, int row, int col1, int col2)
{
	const Color c = pane->term->palette[defbg];
	XftColor xc = { 0, { RED(c) << 8, GREEN(c) << 8, BLUE(c) << 8, 0xffff } };
	int li, ri;
	int xoff, yoff, len;

	if (!BETWEEN(row, 0, pane->term->sb->rows + 1) || (!line))
		return;

	li = MIN(getCharCnt(line->str, MIN(col1, col2)).index, u32slen(line->str));
	ri = MIN(getCharCnt(line->str, MAX(col1, col2)).index, u32slen(line->str));

	xoff = pane->xpad + getCharCnt(line->str, MIN(col1, col2)).col * pane->xfont->cw;
	yoff = pane->ypad + row * pane->xfont->ch;
	len = MIN(u32slen(line->str + li), ri - li);

	XSetForeground(pane->disp, pane->gc, BELLCOLOR(pane->term->palette[deffg]));
	XFillRectangle(pane->disp, pane->pixmap, pane->gc, xoff, yoff,
			u32swidth(line->str + li, len) * pane->xfont->cw, pane->xfont->ch);
	drawXFontString(pane->draw, &xc, pane->xfont, 0, xoff, yoff + pane->xfont->ascent,
			line->str + li, len);
}

void
bell(void *vp)
{
	Pane *pane = (Pane *)vp;
	pane->timer_active[BELL_TIMER] = 1;
	pane->timers[BELL_TIMER] = pane->now;
}
