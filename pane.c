#include <stdio.h>
#include <X11/Xresource.h>

#include "pane.h"
#include "util.h"

/*
 * Pane
 *
 * Pixmapを持ち、そこに端末の内容を書く
 */

#define BLEND_COLOR(c1, a1, c2, a2) (\
		((int)(ALPHA(c1) * (a1) + ALPHA(c2) * (a2)) << 24) +\
		((int)(  RED(c1) * (a1) +   RED(c2) * (a2)) << 16) +\
		((int)(GREEN(c1) * (a1) + GREEN(c2) * (a2)) <<  8) +\
		((int)( BLUE(c1) * (a1) +  BLUE(c2) * (a2)) <<  0))
#define BELLCOLOR(c)    (now < d->bell_time ? BLEND_COLOR((c), 0.925, 0xffffffff, 0.075) : (c))
#define SCROLLMAX(sb)   ((sb)->firstline - MAX((sb)->totallines - (sb)->maxlines, 0))
#define NEW_LINE(d, n)  ((d)->new_lines[(n) + 1])
#define OLD_LINE(d, n)  ((d)->old_lines[(n) + 1])
const long long blink_duration = 800 * 1000 * 1000;
const long long rapid_duration = 200 * 1000 * 1000;
const long long caret_duration = 500 * 1000 * 1000;

static void drawLine(Drawing *, Line *, int, int, int, int, nsec);
static void drawCursor(Drawing *, Line *, int, int, int, nsec);
static void freePixmap(Drawing *);
static void createPixmap(Drawing *, int, int);
static void clearPixmap(Drawing *, nsec);
static void resizeLinebuf(Drawing *);

Pane *
createPane(DispInfo *dinfo, XFont *xfont, int w, int h, int xpad, int ypad, Term *term)
{
	Pane *pane = xmalloc(sizeof(Pane));

	*pane = (Pane){
		.term = term,
		.d = {
			.dinfo = dinfo, .xfont = xfont, .depth = 32,
			.width = w, .height = h, .xpad = xpad, .ypad = ypad,
			.rows = term->sb->rows, .cols = term->sb->cols,
		},
	};
	memset(&pane->d.timer_active, 0, TIMER_NUM);
	memcpy(pane->d.palette, term->palette, PALETTE_SIZE * sizeof(Color));

	/* Paneで対応しているモード */
	term->decmode[25]   = 1;        /* Show cursor */

	/* 描画の準備 */
	createPixmap(&pane->d, w, h);
	resizeLinebuf(&pane->d);
	clearPixmap(&pane->d, pane->d.time_b);

	return pane;
}

void
destroyPane(Pane *pane)
{
	Line **plines;

	freePixmap(&pane->d);
	for (plines = pane->d.new_lines; *plines; plines++)
		freeLine(*plines);
	free(pane->d.new_lines);
	for (plines = pane->d.old_lines; *plines; plines++)
		freeLine(*plines);
	free(pane->d.old_lines);
	free(pane);
}

void
setPaneSize(Drawing *d, int width, int height)
{
	d->width = width;
	d->height = height;
	d->rows = (height - d->ypad * 2) / d->xfont->ch;
	d->cols = (width  - d->xpad * 2) / d->xfont->cw;
	freePixmap(d);
	createPixmap(d, width, height);
	resizeLinebuf(d);
	clearPixmap(d, d->time_b);
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
			(event->xbutton.x - pane->d.xpad) / pane->d.xfont->cw,
			(event->xbutton.y - pane->d.ypad) / pane->d.xfont->ch);
}

void
scrollPane(Drawing *d, int n)
{
	d->scr += n;
}

void
selectPane(Pane *pane, int row, int col, bool start, bool rect)
{
	/* スクロールの境界チェック */
	pane->d.scr = CLIP(pane->d.scr, 0, SCROLLMAX(pane->term->sb));

	setSelection(&pane->sel, pane->term->sb, row - pane->d.scr, col, start, rect);
}

nsec
getNextTime(Drawing *d, nsec now)
{
	nsec time = (nsec)2 << 32;

	/* ベルの時間 */
	if (now < d->bell_time)
		time = d->bell_time - now;

#define wait(t, d)      ((d) - (now - (t)) % (d))
	/* 点滅の時刻 */
	if (d->timer_active[BLINK_TIMER])
		time = MIN(wait(0, blink_duration), time);
	if (d->timer_active[RAPID_TIMER])
		time = MIN(wait(0, rapid_duration), time);
	if (d->timer_active[CARET_TIMER])
		time = MIN(wait(d->caret_time, caret_duration), time);
#undef wait

	return time;
}

void
snapshot(Pane *pane, nsec now)
{
	const nsec bell_duration = 150 * 1000 * 1000;

	/* カーソルの情報を取得 */
	pane->d.cx = pane->term->cx;
	pane->d.cy = pane->term->cy;
	pane->d.ctype = pane->term->ctype;
	pane->d.DECTCEM = DECMODE(pane->term, 25) ||
	                  pane->term->decmode[25] == 0;

	/* スクロールの境界チェック */
	pane->d.scr = CLIP(pane->d.scr, 0, SCROLLMAX(pane->term->sb));

	/* ベルやパレットの更新をチェック */
	if (pane->d.bell_cnt != pane->term->bell_cnt) {
		pane->d.clear_flag |= pane->d.bell_time <= now;
		pane->d.bell_time = now + bell_duration;
		pane->d.bell_cnt = pane->term->bell_cnt;
	}
	if (pane->d.palette_cnt != pane->term->palette_cnt) {
		pane->d.clear_flag = true;
		memcpy(pane->d.palette, pane->term->palette, PALETTE_SIZE * sizeof(Color));
		pane->d.palette_cnt = pane->term->palette_cnt;
	}

	/* バッファの切り替えや行の追加を見てスクロール量を更新 */
	pane->d.scr = (pane->prevbuf != pane->term->sb) ? 0 : pane->d.scr;
	pane->d.scr += (0 < pane->d.scr) ? pane->term->sb->firstline - pane->d.prevfst : 0;
	pane->d.scr = CLIP(pane->d.scr, 0, SCROLLMAX(pane->term->sb));
	pane->prevbuf = pane->term->sb;
	pane->d.prevfst = pane->term->sb->firstline;

	/* 選択範囲をチェックして変更があったら解除 */
	if (pane->sel.sb == pane->term->sb && checkSelection(&pane->sel)) {
		pane->sel.aline = pane->sel.bline;
		pane->sel.acol  = pane->sel.bcol;
	}

	/* 端末の内容を取得 */
	getLines(pane->term->sb, pane->d.new_lines, pane->d.rows + 3,
			pane->d.scr + 1, &pane->sel);

	/* -1行目は画面端をまたいで選択してる場合だけ書く */
	if ((pane->sel.aline < pane->term->sb->firstline - pane->d.scr) ==
	    (pane->sel.bline < pane->term->sb->firstline - pane->d.scr) ||
	     pane->term->sb != pane->sel.sb)
		PUT_NUL(NEW_LINE(&pane->d, -1), 0);
}

int
drawPane(Drawing *d, nsec now, Line *peline, int pecaret)
{
	Line *line;
	int pepos, pewidth, pecaretpos, caretrow;
	int width, width_b;
	int i;

	/* --- タイマーの処理 --- */

	/* 点滅させる必要がないときはキャレットのタイマーを止める */
	d->timer_active[CARET_TIMER] = d->focus &&
		(d->cy + d->scr <= d->rows) &&
		(!d->ctype || d->ctype % 2);

	/* ベルの消灯時刻をまたいでいたら画面クリア */
	if (d->time_b < d->bell_time && d->bell_time <= now)
		d->clear_flag = true;

	d->time_b = now;

	/* --- 端末の内容を描画 --- */

	if (d->clear_flag)
		/* 画面全体を消去する */
		clearPixmap(d, now);
	else
		/* カーソルやPreeditを書く前の状態に戻す */
		XCopyArea(d->dinfo->disp, d->pixbuf, d->pixmap, d->gc,
				d->clear_x, d->clear_y,
				d->clear_w, d->clear_h,
				d->clear_x, d->clear_y);

	/* 次回の消去範囲を設定 */
	d->clear_x = d->xpad + d->xfont->cw * (d->cx - 0.5);
	d->clear_y = d->ypad + d->xfont->ch * (d->cy + d->scr);
	d->clear_w = d->xfont->cw * 2;
	d->clear_h = d->xfont->ch;
	d->clear_flag = false;

	/* 点滅中フラグを一旦クリア */
	d->timer_active[BLINK_TIMER] = d->timer_active[RAPID_TIMER] = false;

	/* Pixmapに書く */
	for (i = -1; i < d->rows + 2; i++) {
		line = NEW_LINE(d, i);

		/* 前回の方が長い場合の塗りつぶし */
		width   = line ? u32swidth(line->str) : 0;
		width_b = u32swidth(OLD_LINE(d, i)->str) + 1;
		if (width < width_b) {
			XSetForeground(d->dinfo->disp, d->gc,
					BELLCOLOR(d->palette[defbg]));
			XFillRectangle(d->dinfo->disp, d->pixmap, d->gc,
					d->xpad + d->xfont->cw * width,
					d->ypad + d->xfont->ch * i,
					d->xfont->cw * (width_b - width),
					d->xfont->ch);
		}

		/* 行を書く */
		if (line)
			drawLine(d, line, i, 0, d->cols + 2, 0, now);
	}

	/* 書いた文字とPixmapの状態を記録 */
	for (i = -1; i < d->rows + 2; i++)
		linecpy(OLD_LINE(d, i), NEW_LINE(d, i));
	XCopyArea(d->dinfo->disp, d->pixmap, d->pixbuf, d->gc,
			0, 0, d->width, d->height, 0, 0);

	/* --- カーソル/Preeditの描画 --- */

	XSetForeground(d->dinfo->disp, d->gc, d->palette[deffg]);
	if (peline && u32slen(peline->str)) {
		/* Preeditの幅とキャレットのPreedit内での位置を取得 */
		pewidth = u32swidth(peline->str);
		pecaretpos = u32snwidth(peline->str, pecaret);

		/* Preeditの画面上での描画位置を決める */
		pepos = d->cols / 2 - pecaretpos;
		pepos = MIN(pepos, 0);
		pepos = MAX(pepos, d->cols - pewidth);
		pepos = MIN(pepos, d->cx);

		/* Preeditとカーソルの描画 */
		drawLine(d, peline, d->cy, pepos, pewidth, 0, now);
		drawCursor(d, peline, d->cy, pepos + pecaretpos, 6, now);

		/* 次回の消去範囲を変更 */
		d->clear_x = d->xpad + d->xfont->cw * (pepos - 0.5);
		d->clear_w = d->xfont->cw * (pewidth + 1);
	} else if (d->DECTCEM && d->cx < d->cols + 2) {
		/* カーソルの描画 */
		caretrow = d->cy + d->scr;
		line = NEW_LINE(d, caretrow);
		if (caretrow <= d->rows)
			drawCursor(d, line, caretrow, d->cx, d->ctype, now);
	}

	return 1;
}

void
drawLine(Drawing *d, Line *line, int row, int col, int width, int pos, nsec now)
{
	int next, i = getIndex(line->str, pos);
	int x, y, w;
	int attr, blink, rapid;
	XftColor xc;
	Color fg, bg, fc, bc;
	int n;

	if (width <= pos || line->str[i] == L'\0')
		return;

	/* 同じ属性の文字はまとめて処理する */
	next = findNextSGR(line, i);
	drawLine(d, line, row, col, width, pos + u32snwidth(&line->str[i], next - i), now);

	/* 座標 */
	x = d->xpad + (col + pos) * d->xfont->cw;
	y = d->ypad + row * d->xfont->ch;
	w = d->xfont->cw * u32snwidth(&line->str[i], next - i);

	/* 変化無し・コピー・書き直しの分岐 */
#define LINE_CMP(R) linecmp(line, OLD_LINE(d, R), pos, next - i)
	if (line->attr[i] & (ITALIC | BLINK | RAPID))
		goto skip;
	if (BETWEEN(row, -1, d->rows + 2) && LINE_CMP(row))
		return;
	for (n = 0; n < d->rows; n++) {
		if (!LINE_CMP(n))
			continue;
		XCopyArea(d->dinfo->disp, d->pixbuf, d->pixmap, d->gc,
				x, d->ypad + n * d->xfont->ch,
				w, d->xfont->ch, x, y);
		return;
	}
skip:
#undef LINE_CMP

	/* 前処理 */
	fg = line->attr[i] & NEGA ? line->bg[i] : line->fg[i];  /* 反転 */
	bg = line->attr[i] & NEGA ? line->fg[i] : line->bg[i];
	if (line->attr[i] & BOLD)                               /* 太字 */
		fg += fg < 8 ? 8 : 0;
	fc = fg < PALETTE_SIZE ? d->palette[fg] : fg;           /* 色を取得 */
	bc = bg < PALETTE_SIZE ? d->palette[bg] : bg;
	if (line->attr[i] & FAINT)                              /* 細字 */
		fc = BLEND_COLOR(fc, 0.6, bc, 0.4);

	/* 背景を塗る */
	XSetForeground(d->dinfo->disp, d->gc, BELLCOLOR(bc));
	XFillRectangle(d->dinfo->disp, d->pixmap, d->gc, x, y, w, d->xfont->ch);

	/* 非表示・点滅 */
	d->timer_active[BLINK_TIMER] |= line->attr[i] & BLINK;
	d->timer_active[RAPID_TIMER] |= line->attr[i] & RAPID;
	blink = line->attr[i] & BLINK ? ((now / blink_duration) % 2) ? 2 : 0 : 1;
	rapid = line->attr[i] & RAPID ? ((now / rapid_duration) % 2) ? 2 : 0 : 1;
	if (line->attr[i] & CONCEAL || blink + rapid < 2)
		return;

	y += d->xfont->ascent;

	/* 色をXftColorに変換 */
	xc.color.red   =   RED(fc) << 8;
	xc.color.green = GREEN(fc) << 8;
	xc.color.blue  =  BLUE(fc) << 8;
	xc.color.alpha = 0xffff;

	/* 文字を書く */
	attr = FONT_NONE;
	attr |= line->attr[i] & BOLD   ? FONT_BOLD   : FONT_NONE;
	attr |= line->attr[i] & ITALIC ? FONT_ITALIC : FONT_NONE;
	drawXFontString(d->draw, &xc, d->xfont, attr, x, y, w + d->xfont->cw,
			&line->str[i], next - i);

	/* 後処理 */
	XSetForeground(d->dinfo->disp, d->gc, fc);
	if (line->attr[i] & (ULINE | DULINE))   /* 下線 */
		XDrawLine(d->dinfo->disp, d->pixmap, d->gc, x, y + 1, x + w - 1, y + 1);
	if (line->attr[i] & DULINE)             /* 二重下線 */
		XDrawLine(d->dinfo->disp, d->pixmap, d->gc, x, y + 3, x + w - 1, y + 3);
	y -= d->xfont->ascent * 0.4;         /* 取消 */
	if (line->attr[i] & STRIKE)
		XDrawLine(d->dinfo->disp, d->pixmap, d->gc, x, y + 1, x + w - 1, y + 1);
}

void
drawCursor(Drawing *d, Line *line, int row, int col, int type, nsec now)
{
	int index, col2, width;
	getCharCnt(line->str, col, &index, &col2, &width);
	char32_t *c = index < u32slen(line->str) ? &line->str[index] : (char32_t *)L" ";
	const int x = d->xpad + col * d->xfont->cw;
	const int y = d->ypad + row * d->xfont->ch;
	const int cw = d->xfont->cw * width - 1;
	const int ch = d->xfont->ch;
	const DispInfo *dinfo = d->dinfo;
	int attr;
	Line cursor;

	/* 点滅 */
	if ((!type || type % 2) && d->focus &&
	    ((now - d->caret_time) / caret_duration) % 2)
		return;

	XSetForeground(dinfo->disp, d->gc, BELLCOLOR(d->palette[deffg]));

	switch (type) {
	default: case 0: case 1: case 2: /* ブロック */
		if (d->focus) {
			attr = index < u32slen(line->str) ? line->attr[index] : 0;
			cursor = (Line){c, &attr, &defbg, &deffg};
			drawLine(d, &cursor, row, col2, 1, 0, now);
		} else {
			XDrawRectangle(dinfo->disp, d->pixmap, d->gc, x, y, cw, ch - 1);
			XDrawPoint(dinfo->disp, d->pixmap, d->gc, x + cw, y + ch - 1);
		}
		break;
	case 3: case 4: /* 下線 */
		XFillRectangle(dinfo->disp, d->pixmap, d->gc,
				x, y + 1 + d->xfont->ascent, cw, ch * 0.1);
		break;
	case 5: case 6: /* 縦線 */
		XFillRectangle(dinfo->disp, d->pixmap, d->gc,
				x - 1, y, ch * 0.1, ch);
		break;
	}

	/* 次回の消去範囲を変更 */
	d->clear_x = d->xpad + d->xfont->cw * (col2 - 0.5);
	d->clear_w = cw + d->xfont->cw;
}

void
freePixmap(Drawing *d)
{
	XftDrawDestroy(d->draw);
	XFreeGC(d->dinfo->disp, d->gc);
	XFreePixmap(d->dinfo->disp, d->pixmap);
	XFreePixmap(d->dinfo->disp, d->pixbuf);
}

void
createPixmap(Drawing *d, int w, int h)
{
	const DispInfo *i = d->dinfo;

	d->pixmap = XCreatePixmap(i->disp, i->root, w, h, d->depth);
	d->pixbuf = XCreatePixmap(i->disp, i->root, w, h, d->depth);
	d->gc = XCreateGC(i->disp, d->pixmap, 0, NULL);
	XSetGraphicsExposures(i->disp, d->gc, false);
	d->draw = XftDrawCreate(i->disp, d->pixmap, i->visual, i->cmap);
}

void
clearPixmap(Drawing *d, nsec now)
{
	int i;

	XSetForeground(d->dinfo->disp, d->gc, BELLCOLOR(d->palette[defbg]));
	XFillRectangle(d->dinfo->disp, d->pixmap, d->gc, 0, 0, d->width, d->height);
	XSetForeground(d->dinfo->disp, d->gc, BELLCOLOR(d->palette[defbg]));
	XFillRectangle(d->dinfo->disp, d->pixbuf, d->gc, 0, 0, d->width, d->height);

	for (i = 0; i < d->rows + 3; i++)
		PUT_NUL(d->old_lines[i], 0);
}

void
resizeLinebuf(Drawing *d)
{
	Line **plines;
	int i;

	if (d->new_lines)
		for (plines = d->new_lines; *plines; plines++)
			freeLine(*plines);
	if (d->old_lines)
		for (plines = d->old_lines; *plines; plines++)
			freeLine(*plines);
	d->new_lines = xrealloc(d->new_lines, (d->rows + 4) * sizeof(Line *));
	d->new_lines[d->rows + 3] = NULL;
	d->old_lines = xrealloc(d->old_lines, (d->rows + 4) * sizeof(Line *));
	d->old_lines[d->rows + 3] = NULL;
	for (i = 0; i < d->rows + 3; i++) {
		d->new_lines[i] = allocLine();
		d->old_lines[i] = allocLine();
	}
}
