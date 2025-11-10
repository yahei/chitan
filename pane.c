#include "pane.h"
#include "util.h"

/*
 * Pane
 *
 * Pixmapを持ち、そこに端末の内容を書く
 */

#define CHOOSE(a, b, c) (timespeccmp((a), (b), c) ? (a) : (b))
#define BLEND_COLOR(c1, a1, c2, a2) (\
		((int)(ALPHA(c1) * (a1) + ALPHA(c2) * (a2)) << 24) +\
		((int)(  RED(c1) * (a1) +   RED(c2) * (a2)) << 16) +\
		((int)(GREEN(c1) * (a1) + GREEN(c2) * (a2)) <<  8) +\
		((int)( BLUE(c1) * (a1) +  BLUE(c2) * (a2)) <<  0))
#define BELLCOLOR(c)    (pane->bell_lit ?\
		BLEND_COLOR((c), 0.85, 0xffffffff, 0.15) : (c))
#define SCROLLMAX(sb)   ((sb)->firstline - MAX((sb)->totallines - (sb)->maxlines, 0))
#define CREATE_PIXMAP(i,w,h,d)  XCreatePixmap(i->disp, DefaultRootWindow(i->disp), w, h, d)
#define DRAW_CREATE(i,p)        XftDrawCreate(i->disp, p, i->visual, i->cmap)

static void drawLine(Pane *, Line *, int, int, int, int);
static int linecmp(Pane *, Line *, Line *, int, int);
static void drawCursor(Pane *, Line *, int, int, int);
static void resizeLines(Pane *);
static void clearPixmap(Pane *);

Pane *
createPane(DispInfo *dinfo, XFont *xfont, int width, int height, float alpha, int lines, char *const cmd[])
{
	Pane *pane = xmalloc(sizeof(Pane));

	*pane = (Pane){
		.dinfo = dinfo, .xfont = xfont, .depth = 32, .alpha = alpha,
		.width = width, .height = height,
		.xpad = xfont->cw / 2, .ypad = xfont->cw / 2
	};
	memset(&pane->timers, 0, TIMER_NUM * sizeof(struct timespec));
	memset(&pane->timer_active, 0, TIMER_NUM);
	memset(&pane->timer_lit, 0, TIMER_NUM);

	/* 端末をオープン */
	pane->term = openTerm((height - pane->ypad * 2) / xfont->ch,
			(width - pane->xpad * 2) / xfont->cw, lines, cmd[0], cmd);
	if (!pane->term)
		errExit("openTerm failed.\n");
	pane->term->palette[defbg] = ((0xff & (int)(0xff * alpha)) << 24) +
		(0x00ffffff & pane->term->palette[defbg]);

	/* 描画の準備 */
	pane->pixmap = CREATE_PIXMAP(pane->dinfo, width, height, pane->depth);
	pane->pixbuf = CREATE_PIXMAP(pane->dinfo, width, height, pane->depth);
	pane->gc = XCreateGC(dinfo->disp, pane->pixmap, 0, NULL);
	pane->draw = DRAW_CREATE(pane->dinfo, pane->pixmap);
	pane->lines = xmalloc(sizeof(Line *));
	*pane->lines = NULL;
	pane->lines_b = xmalloc(sizeof(Line *));
	*pane->lines_b = NULL;
	resizeLines(pane);
	clearPixmap(pane);

	return pane;
}

void
destroyPane(Pane *pane)
{
	Line **plines;

	closeTerm(pane->term);
	XftDrawDestroy(pane->draw);
	XFreeGC(pane->dinfo->disp, pane->gc);
	XFreePixmap(pane->dinfo->disp, pane->pixmap);
	XFreePixmap(pane->dinfo->disp, pane->pixbuf);
	for (plines = pane->lines; *plines; plines++)
		freeLine(*plines);
	free(pane->lines);
	for (plines = pane->lines_b; *plines; plines++)
		freeLine(*plines);
	free(pane->lines_b);
	free(pane);
}

void
setPaneSize(Pane *pane, int width, int height)
{
	int oldrows = pane->term->sb->rows;
	Line **plines;

	XftDrawDestroy(pane->draw);
	XFreeGC(pane->dinfo->disp, pane->gc);
	XFreePixmap(pane->dinfo->disp, pane->pixmap);
	XFreePixmap(pane->dinfo->disp, pane->pixbuf);

	pane->pixmap = CREATE_PIXMAP(pane->dinfo, width, height, pane->depth);
	pane->pixbuf = CREATE_PIXMAP(pane->dinfo, width, height, pane->depth);
	pane->gc = XCreateGC(pane->dinfo->disp, pane->pixmap, 0, NULL);
	pane->draw = DRAW_CREATE(pane->dinfo, pane->pixmap);
	pane->width = width;
	pane->height = height;

	setWinSize(pane->term, (height - pane->ypad * 2) / pane->xfont->ch,
			(width - pane->xpad * 2) / pane->xfont->cw, width, height);
	if (oldrows == pane->term->sb->rows)
		for (plines = pane->lines_b; *plines; plines++)
			PUT_NUL(*plines, 0);
	else
		resizeLines(pane);
	clearPixmap(pane);
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
	pane->redraw_flag = true;
	pane->scr += n;
	pane->scr = CLIP(pane->scr, 0, SCROLLMAX(pane->term->sb));
}

void
selectPane(Pane *pane, int row, int col, bool start, bool rect)
{
	pane->redraw_flag = true;
	setSelection(&pane->sel, pane->term->sb, row - pane->scr, col, start, rect);
}

void
manageTimer(Pane *pane, const struct timespec *now)
{
	static const struct timespec duration[] = {
		[BLINK_TIMER] = { 0, 800 * 1000 * 1000 },
		[RAPID_TIMER] = { 0, 200 * 1000 * 1000 },
		[CARET_TIMER] = { 0, 500 * 1000 * 1000 },
	};
	Line **plines;
	int i;

	/* 点滅させる必要がないときはキャレットのタイマーを止める */
	pane->timer_active[CARET_TIMER] = pane->focus &&
		(pane->term->cy + pane->scr <= pane->term->sb->rows) &&
		(!pane->term->ctype || pane->term->ctype % 2);

	/* 点滅の処理 */
	for (i = 0; i < TIMER_NUM; i++) {
		if (pane->timer_active[i] && timespeccmp(&pane->timers[i], now, <)) {
			pane->timer_lit[i] = !pane->timer_lit[i];
			pane->redraw_flag = true;
			timespecadd(&pane->timers[i], &duration[i], &pane->timers[i]);
			if (timespeccmp(&pane->timers[i], now, <=))
				timespecadd(now, &duration[i], &pane->timers[i]);
		}
	}

	/* ベルの処理 */
	if (timespeccmp(now, &pane->bell_time, <) != pane->bell_lit) {
		/* 点灯状態が変化したら画面全体を書き直す */
		pane->bell_lit ^= true;
		pane->redraw_flag = true;
		for (plines = pane->lines_b; *plines; plines++)
			PUT_NUL(*plines, 0);
		clearPixmap(pane);
	}
}

void
getNextTime(Pane *pane, struct timespec *timeout, const struct timespec *now)
{
	struct timespec nexttime = (struct timespec){ 1 << 16, 0 };
	int i;

	timespecadd(now, &nexttime, &nexttime);

	/* 点滅の時刻 */
	for (i = 0; i < TIMER_NUM; i++)
		if (pane->timer_active[i] &&
		    timespeccmp(now, &pane->timers[i], <) &&
		    timespeccmp(&pane->timers[i], &nexttime, <))
			nexttime = pane->timers[i];

	/* ベルの時刻 */
	if (timespeccmp(now, &pane->bell_time, <) &&
	    timespeccmp(&pane->bell_time, &nexttime, <))
		nexttime = pane->bell_time;

	timespecsub(&nexttime, now, timeout);
}

void
drawPane(Pane *pane, const struct timespec *now, Line *peline, int pecaret)
{
	struct timespec bell_duration = { 0, 150 * 1000 * 1000 };
	Line *line, **plines;
	int pepos, pewidth, pecaretpos, caretrow;
	int width, width_b;
	int i;

	/* タイマーの処理 */
	manageTimer(pane, now);

	if (!pane->redraw_flag)
		return;

	/* スクロール量の更新 */
	pane->scr = (pane->prevbuf != pane->term->sb) ? 0 : pane->scr;
	pane->scr += (0 < pane->scr) ? pane->term->sb->firstline - pane->prevfst : 0;
	pane->scr = CLIP(pane->scr, 0, SCROLLMAX(pane->term->sb));
	pane->prevbuf = pane->term->sb;
	pane->prevfst = pane->term->sb->firstline;

	/* ベルとパレット変更のチェック */
	if (pane->bell_cnt != pane->term->bell_cnt) {
		timespecadd(now, &bell_duration, &pane->bell_time);
		pane->bell_cnt = pane->term->bell_cnt;
	}
	if (pane->pallet_cnt != pane->term->pallet_cnt) {
		clearPixmap(pane);
		for (plines = pane->lines_b; *plines; plines++)
			PUT_NUL(*plines, 0);
		pane->pallet_cnt = pane->term->pallet_cnt;
	}

	/* 選択範囲のチェック */
	if ((pane->sel.aline != pane->sel.bline || pane->sel.acol != pane->sel.bcol) &&
			pane->sel.sb == pane->term->sb)
		checkSelection(&pane->sel);

	/* 画面をクリア */
	/* カーソル等を書く前の状態に戻す */
	XCopyArea(pane->dinfo->disp, pane->pixbuf, pane->pixmap, pane->gc,
			pane->cx_b, pane->cy_b,
			pane->cw_b, pane->ch_b,
			pane->cx_b, pane->cy_b);

	/* 次回の消去範囲を設定 */
	pane->cx_b = pane->xpad + pane->xfont->cw * (pane->term->cx - 0.5);
	pane->cy_b = pane->ypad + pane->xfont->ch * (pane->term->cy + pane->scr);
	pane->cw_b = pane->xfont->cw * 2;
	pane->ch_b = pane->xfont->ch;

	/* 点滅中フラグをクリア */
	pane->timer_active[BLINK_TIMER] = pane->timer_active[RAPID_TIMER] = false;

	/* 端末の内容をPixmapに書く */
	getLines(pane->term->sb, pane->lines, pane->term->sb->rows + 2, pane->scr, &pane->sel);
	for (i = 0; i < pane->term->sb->rows + 2; i++) {
		line = pane->lines[i];

		/* 前回の方が長い場合の塗りつぶし */
		width   = line ? u32swidth(line->str) : 0;
		width_b = u32swidth(pane->lines_b[i]->str);
		if (width < width_b) {
			XSetForeground(pane->dinfo->disp, pane->gc, BELLCOLOR(pane->term->palette[defbg]));
			XFillRectangle(pane->dinfo->disp, pane->pixmap, pane->gc,
					pane->xpad + pane->xfont->cw * width,
					pane->ypad + pane->xfont->ch * i,
					pane->xfont->cw * (width_b - width),
					pane->xfont->ch);
		}

		/* 行を書く */
		if (line)
			drawLine(pane, line, i, 0, pane->term->sb->cols + 2, 0);
	}

	/* 書いた文字とPixmapの状態を記録 */
	for (i = 0; i < pane->term->sb->rows + 2; i++)
		linecpy(pane->lines_b[i], pane->lines[i]);
	XCopyArea(pane->dinfo->disp, pane->pixmap, pane->pixbuf, pane->gc,
			0, 0, pane->width, pane->height, 0, 0);

	/* カーソルかPreeditを表示 */
	XSetForeground(pane->dinfo->disp, pane->gc, pane->term->palette[deffg]);
	if (u32slen(peline->str)) {
		/* Preeditの幅とキャレットのPreedit内での位置を取得 */
		pewidth = u32swidth(peline->str);
		pecaretpos = u32snwidth(peline->str, pecaret);

		/* Preeditの画面上での描画位置を決める */
		pepos = pane->term->cx - pecaretpos;
		pepos = MIN(pepos, 0);
		pepos = MAX(pepos, pane->term->sb->cols - pewidth);
		pepos = MIN(pepos, pane->term->cx);

		/* Preeditとカーソルの描画 */
		drawLine(pane, peline, pane->term->cy, pepos, pewidth, 0);
		drawCursor(pane, peline, pane->term->cy, pepos + pecaretpos, 6);

		/* 次回の消去範囲を変更 */
		pane->cx_b = pane->xpad + pane->xfont->cw * (pepos - 0.5);
		pane->cw_b = pane->xfont->cw * (pewidth + 1);
	} else if (1 <= pane->term->dec[25] && pane->term->cx < pane->term->sb->cols + 2) {
		/* カーソルの描画 */
		caretrow = pane->term->cy + pane->scr;
		if (caretrow <= pane->term->sb->rows && (line = getLine(pane->term->sb, pane->term->cy)))
			drawCursor(pane, line, caretrow, pane->term->cx, pane->term->ctype);
	}

	pane->redraw_flag = false;
}

void
drawLine(Pane *pane, Line *line, int row, int col, int width, int pos)
{
	int next, i = getCharCnt(line->str, pos).index;
	int x, y, w;
	int attr, fg, bg;
	XftColor xc;
	Color fc, bc;
	int sl;

	if (width <= pos || line->str[i] == L'\0')
		return;

	/* 同じ属性の文字はまとめて処理する */
	next = MIN(findNextSGR(line, i), width);
	drawLine(pane, line, row, col, width, pos + u32snwidth(&line->str[i], next - i));

	/* 座標 */
	x = pane->xpad + (col + pos) * pane->xfont->cw;
	y = pane->ypad + row * pane->xfont->ch;
	w = pane->xfont->cw * u32snwidth(&line->str[i], next - i);

	/* 変化無し・コピー・書き直しの分岐 */
#define LINE_CMP(R) linecmp(pane, line, pane->lines_b[R], pos, next - i)
	if (line->attr[i] & (ITALIC | BLINK | RAPID))
		sl = pane->term->sb->rows;
	else if (LINE_CMP(row))
		return;
	else
		for (sl = 0; sl < pane->term->sb->rows; sl++)
			if (LINE_CMP(sl))
				break;
	if (sl < pane->term->sb->rows) {
		XCopyArea(pane->dinfo->disp, pane->pixbuf, pane->pixmap, pane->gc,
				x, pane->ypad + (sl) * pane->xfont->ch,
				w, pane->xfont->ch, x, y);
		return;
	}
#undef LINE_CMP

	/* 前処理 */
	fg = line->attr[i] & NEGA ? line->bg[i] : line->fg[i];  /* 反転 */
	bg = line->attr[i] & NEGA ? line->fg[i] : line->bg[i];
	if (line->attr[i] & BOLD)                               /* 太字 */
		fg += fg < 8 ? 8 : 0;
	fc = pane->term->palette[fg];                           /* 色を取得 */
	bc = pane->term->palette[bg];
	if (line->attr[i] & FAINT)                              /* 細字 */
		fc = BLEND_COLOR(fc, 0.6, bc, 0.4);

	/* 背景を塗る */
	XSetForeground(pane->dinfo->disp, pane->gc, BELLCOLOR(bc));
	XFillRectangle(pane->dinfo->disp, pane->pixmap, pane->gc, x, y, w, pane->xfont->ch);

	y += pane->xfont->ascent;

	/* 点滅 */
	if (line->attr[i] & BLINK)
		pane->timer_active[BLINK_TIMER] = true;
	if (line->attr[i] & RAPID)
		pane->timer_active[RAPID_TIMER] = true;
	if ((line->attr[i] & BLINK ? pane->timer_lit[BLINK_TIMER] ? 2 : 0 : 1) +
	    (line->attr[i] & RAPID ? pane->timer_lit[RAPID_TIMER] ? 2 : 0 : 1) < 2)
		return;

	/* 非表示 */
	if (line->attr[i] & CONCEAL)
		return;

	/* 色をXftColorに変換 */
	xc.color.red   =   RED(fc) << 8;
	xc.color.green = GREEN(fc) << 8;
	xc.color.blue  =  BLUE(fc) << 8;
	xc.color.alpha = 0xffff;

	/* 文字を書く */
	attr = FONT_NONE;
	attr |= line->attr[i] & BOLD   ? FONT_BOLD   : FONT_NONE;
	attr |= line->attr[i] & ITALIC ? FONT_ITALIC : FONT_NONE;
	drawXFontString(pane->draw, &xc, pane->xfont, attr, x, y, pane->width,
			&line->str[i], next - i);

	/* 後処理 */
	XSetForeground(pane->dinfo->disp, pane->gc, fc);
	if (line->attr[i] & (ULINE | DULINE))   /* 下線 */
		XDrawLine(pane->dinfo->disp, pane->pixmap, pane->gc, x, y + 1, x + w - 1, y + 1);
	if (line->attr[i] & DULINE)             /* 二重下線 */
		XDrawLine(pane->dinfo->disp, pane->pixmap, pane->gc, x, y + 3, x + w - 1, y + 3);
	y -= pane->xfont->ascent * 0.4;         /* 取消 */
	if (line->attr[i] & STRIKE)
		XDrawLine(pane->dinfo->disp, pane->pixmap, pane->gc, x, y + 1, x + w - 1, y + 1);
}

int
linecmp(Pane *pane, Line *line1, Line *line2, int pos, int len)
{
	CharCnt cc1 = getCharCnt(line1->str, pos);
	CharCnt cc2 = getCharCnt(line2->str, pos);

#define CMP(A,T) !memcmp(&line1->A[cc1.index], &line2->A[cc2.index], len * sizeof(T))
	if (cc2.index + len <= u32slen(line2->str) && cc1.col == cc2.col &&
	    CMP(str, char32_t) && CMP(attr, int) && CMP(fg, int) && CMP(bg, int) &&
	    (cc2.index < 1 || !(line2->attr[cc2.index - 1] & ITALIC)))
			return 1;
	return 0;
#undef CMP
}

void
drawCursor(Pane *pane, Line *line, int row, int col, int type)
{
	const int index = getCharCnt(line->str, col).index;
	char32_t *c = index < u32slen(line->str) ? &line->str[index] : (char32_t *)L" ";
	const int x = pane->xpad + col * pane->xfont->cw;
	const int y = pane->ypad + row * pane->xfont->ch;
	const int cw = pane->xfont->cw * u32snwidth(c, 1) - 1;
	const int ch = pane->xfont->ch;
	const DispInfo *dinfo = pane->dinfo;
	int attr;
	Line cursor;

	/* 点滅 */
	if (pane->focus && (!type || type % 2) && (pane->timer_lit[CARET_TIMER]))
		return;

	XSetForeground(dinfo->disp, pane->gc, BELLCOLOR(pane->term->palette[deffg]));

	switch (type) {
	default: case 0: case 1: case 2: /* ブロック */
		if (pane->focus) {
			attr = index < u32slen(line->str) ? line->attr[index] : 0;
			cursor = (Line){c, &attr, &defbg, &deffg};
			drawLine(pane, &cursor, row, col, 1, 0);
		} else {
			XDrawRectangle(dinfo->disp, pane->pixmap, pane->gc, x, y, cw, ch - 1);
			XDrawPoint(dinfo->disp, pane->pixmap, pane->gc, x + cw, y + ch - 1);
		}
		break;
	case 3: case 4: /* 下線 */
		XFillRectangle(dinfo->disp, pane->pixmap, pane->gc, x, y + 1 + pane->xfont->ascent, cw, ch * 0.1);
		break;
	case 5: case 6: /* 縦線 */
		XFillRectangle(dinfo->disp, pane->pixmap, pane->gc, x - 1, y, ch * 0.1, ch);
		break;
	}

	/* 次回の消去範囲を変更 */
	pane->cw_b = cw + pane->xfont->cw;
}

void
resizeLines(Pane *pane)
{
	Line **plines;
	int i;

	for (plines = pane->lines; *plines; plines++)
		freeLine(*plines);
	for (plines = pane->lines_b; *plines; plines++)
		freeLine(*plines);

	pane->lines = xrealloc(pane->lines, (pane->term->sb->rows + 3) * sizeof(Line *));
	pane->lines[pane->term->sb->rows + 2] = NULL;
	pane->lines_b = xrealloc(pane->lines_b, (pane->term->sb->rows + 3) * sizeof(Line *));
	pane->lines_b[pane->term->sb->rows + 2] = NULL;

	for (i = 0; i < pane->term->sb->rows + 2; i++) {
		pane->lines[i] = allocLine();
		pane->lines_b[i] = allocLine();
	}
}

void
clearPixmap(Pane *pane)
{
	XSetForeground(pane->dinfo->disp, pane->gc, BELLCOLOR(pane->term->palette[defbg]));
	XFillRectangle(pane->dinfo->disp, pane->pixmap, pane->gc, 0, 0, pane->width, pane->height);
	XSetForeground(pane->dinfo->disp, pane->gc, BELLCOLOR(pane->term->palette[defbg]));
	XFillRectangle(pane->dinfo->disp, pane->pixbuf, pane->gc, 0, 0, pane->width, pane->height);
}
