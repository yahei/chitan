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
#define BELLCOLOR(c)    (now < pane->d.bell_time ? BLEND_COLOR((c), 0.925, 0xffffffff, 0.075) : (c))
#define SCROLLMAX(sb)   ((sb)->firstline - MAX((sb)->totallines - (sb)->maxlines, 0))
#define NEW_LINE(p, n)  (pane->d.new_lines[n + 1])
#define OLD_LINE(p, n)  (pane->d.old_lines[n + 1])
const long long blink_duration = 800 * 1000 * 1000;
const long long rapid_duration = 200 * 1000 * 1000;
const long long caret_duration = 500 * 1000 * 1000;

static void drawLine(Pane *, Line *, int, int, int, int, nsec);
static void drawCursor(Pane *, Line *, int, int, int, nsec);
static void freePixmap(Pane *);
static void createPixmap(Pane *, int, int);
static void clearPixmap(Pane *, nsec);

Pane *
createPane(DispInfo *dinfo, XFont *xfont, int w, int h, int xpad, int ypad, Term *term)
{
	Pane *pane = xmalloc(sizeof(Pane));

	*pane = (Pane){
		.term = term,
		.d = {
			.dinfo = dinfo, .xfont = xfont, .depth = 32,
			.width = w, .height = h, .xpad = xpad, .ypad = ypad,
		},
	};
	memset(&pane->d.timer_active, 0, TIMER_NUM);
	memcpy(pane->d.palette, term->palette, PALETTE_SIZE * sizeof(Color));

	/* 描画の準備 */
	createPixmap(pane, w, h);
	clearPixmap(pane, pane->d.time_b);

	return pane;
}

void
destroyPane(Pane *pane)
{
	Line **plines;

	closeTerm(pane->term);
	freePixmap(pane);
	for (plines = pane->d.new_lines; *plines; plines++)
		freeLine(*plines);
	free(pane->d.new_lines);
	for (plines = pane->d.old_lines; *plines; plines++)
		freeLine(*plines);
	free(pane->d.old_lines);
	free(pane);
}

void
setPaneSize(Pane *pane, int width, int height)
{
	pane->d.width = width;
	pane->d.height = height;
	setWinSize(pane->term, (height - pane->d.ypad * 2) / pane->d.xfont->ch,
			(width - pane->d.xpad * 2) / pane->d.xfont->cw, width, height);
	freePixmap(pane);
	createPixmap(pane, width, height);
	clearPixmap(pane, pane->d.time_b);
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
scrollPane(Pane *pane, int n)
{
	pane->d.redraw_flag = true;
	pane->d.scr += n;
}

void
selectPane(Pane *pane, int row, int col, bool start, bool rect)
{
	/* スクロールの境界チェック */
	pane->d.scr = CLIP(pane->d.scr, 0, SCROLLMAX(pane->term->sb));

	pane->d.redraw_flag = true;
	setSelection(&pane->sel, pane->term->sb, row - pane->d.scr, col, start, rect);
}

nsec
getNextTime(Pane *pane, nsec now)
{
	nsec time = (nsec)2 << 32;

	/* ベルの時間 */
	if (now < pane->d.bell_time)
		time = pane->d.bell_time - now;

#define wait(t, d)      ((d) - (now - (t)) % (d))
	/* 点滅の時刻 */
	if (pane->d.timer_active[BLINK_TIMER])
		time = MIN(wait(0, blink_duration), time);
	if (pane->d.timer_active[RAPID_TIMER])
		time = MIN(wait(0, rapid_duration), time);
	if (pane->d.timer_active[CARET_TIMER])
		time = MIN(wait(pane->d.caret_time, caret_duration), time);
#undef wait

	return time;
}

int
drawPane(Pane *pane, nsec now, Line *peline, int pecaret)
{
	const nsec bell_duration = 150 * 1000 * 1000;
	Line *line;
	int pepos, pewidth, pecaretpos, caretrow;
	int width, width_b;
	bool clear_flag = false;
	int i;

	/* スクロールの境界チェック */
	pane->d.scr = CLIP(pane->d.scr, 0, SCROLLMAX(pane->term->sb));

	/* --- タイマーの処理 --- */

	/* 点滅させる必要がないときはキャレットのタイマーを止める */
	pane->d.timer_active[CARET_TIMER] = pane->d.focus &&
		(pane->term->cy + pane->d.scr <= pane->term->sb->rows) &&
		(!pane->term->ctype || pane->term->ctype % 2);

	/* ベルの消灯時刻をまたいでいたら画面クリア */
	if (pane->d.time_b < pane->d.bell_time && pane->d.bell_time <= now)
		pane->d.redraw_flag = clear_flag = true;

	/* 点滅の切り替わり時刻をまたいでいたら再描画 */
#define LIT(T,D) (((T) / (D)) % 2)
#define CHECK(T,D,B) (pane->d.timer_active[T] && LIT(pane->d.time_b - (B), D) != LIT( now - (B), D))
	if (CHECK(BLINK_TIMER, blink_duration, 0) ||
	    CHECK(RAPID_TIMER, rapid_duration, 0) ||
	    CHECK(CARET_TIMER, caret_duration, pane->d.caret_time))
		pane->d.redraw_flag = true;
#undef CHECK
#undef LIT

	pane->d.time_b = now;

	if (!pane->d.redraw_flag)
		return 0;

	/* --- 描画前の処理 --- */

	/* ベルやパレットの更新をチェック */
	if (pane->d.bell_cnt != pane->term->bell_cnt) {
		clear_flag |= pane->d.bell_time <= now;
		pane->d.bell_time = now + bell_duration;
		pane->d.bell_cnt = pane->term->bell_cnt;
	}
	if (pane->d.palette_cnt != pane->term->palette_cnt) {
		clear_flag = true;
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

	/* --- 端末の内容を描画 --- */

	if (clear_flag)
		/* 画面全体を消去する */
		clearPixmap(pane, now);
	else
		/* カーソルやPreeditを書く前の状態に戻す */
		XCopyArea(pane->d.dinfo->disp, pane->d.pixbuf, pane->d.pixmap, pane->d.gc,
				pane->d.clear_x, pane->d.clear_y,
				pane->d.clear_w, pane->d.clear_h,
				pane->d.clear_x, pane->d.clear_y);

	/* 次回の消去範囲を設定 */
	pane->d.clear_x = pane->d.xpad + pane->d.xfont->cw * (pane->term->cx - 0.5);
	pane->d.clear_y = pane->d.ypad + pane->d.xfont->ch * (pane->term->cy + pane->d.scr);
	pane->d.clear_w = pane->d.xfont->cw * 2;
	pane->d.clear_h = pane->d.xfont->ch;

	/* 端末の内容を取得 */
	getLines(pane->term->sb, pane->d.new_lines, pane->term->sb->rows + 3,
			pane->d.scr + 1, &pane->sel);

	/* -1行目は画面端をまたいで選択してる場合だけ書く */
	if ((pane->sel.aline < pane->term->sb->firstline - pane->d.scr) ==
	    (pane->sel.bline < pane->term->sb->firstline - pane->d.scr) ||
	     pane->term->sb != pane->sel.sb)
		PUT_NUL(NEW_LINE(pane, -1), 0);

	/* 点滅中フラグを一旦クリア */
	pane->d.timer_active[BLINK_TIMER] = pane->d.timer_active[RAPID_TIMER] = false;

	/* Pixmapに書く */
	for (i = -1; i < pane->term->sb->rows + 2; i++) {
		line = NEW_LINE(pane, i);

		/* 前回の方が長い場合の塗りつぶし */
		width   = line ? u32swidth(line->str) : 0;
		width_b = u32swidth(OLD_LINE(pane, i)->str) + 1;
		if (width < width_b) {
			XSetForeground(pane->d.dinfo->disp, pane->d.gc,
					BELLCOLOR(pane->d.palette[defbg]));
			XFillRectangle(pane->d.dinfo->disp, pane->d.pixmap, pane->d.gc,
					pane->d.xpad + pane->d.xfont->cw * width,
					pane->d.ypad + pane->d.xfont->ch * i,
					pane->d.xfont->cw * (width_b - width),
					pane->d.xfont->ch);
		}

		/* 行を書く */
		if (line)
			drawLine(pane, line, i, 0, pane->term->sb->cols + 2, 0, now);
	}

	/* 書いた文字とPixmapの状態を記録 */
	for (i = -1; i < pane->term->sb->rows + 2; i++)
		linecpy(OLD_LINE(pane, i), NEW_LINE(pane, i));
	XCopyArea(pane->d.dinfo->disp, pane->d.pixmap, pane->d.pixbuf, pane->d.gc,
			0, 0, pane->d.width, pane->d.height, 0, 0);

	/* --- カーソル/Preeditの描画 --- */

	XSetForeground(pane->d.dinfo->disp, pane->d.gc, pane->d.palette[deffg]);
	if (u32slen(peline->str)) {
		/* Preeditの幅とキャレットのPreedit内での位置を取得 */
		pewidth = u32swidth(peline->str);
		pecaretpos = u32snwidth(peline->str, pecaret);

		/* Preeditの画面上での描画位置を決める */
		pepos = pane->term->sb->cols / 2 - pecaretpos;
		pepos = MIN(pepos, 0);
		pepos = MAX(pepos, pane->term->sb->cols - pewidth);
		pepos = MIN(pepos, pane->term->cx);

		/* Preeditとカーソルの描画 */
		drawLine(pane, peline, pane->term->cy, pepos, pewidth, 0, now);
		drawCursor(pane, peline, pane->term->cy, pepos + pecaretpos, 6, now);

		/* 次回の消去範囲を変更 */
		pane->d.clear_x = pane->d.xpad + pane->d.xfont->cw * (pepos - 0.5);
		pane->d.clear_w = pane->d.xfont->cw * (pewidth + 1);
	} else if (1 <= pane->term->dec[25] && pane->term->cx < pane->term->sb->cols + 2) {
		/* カーソルの描画 */
		caretrow = pane->term->cy + pane->d.scr;
		line = NEW_LINE(pane, caretrow);
		if (caretrow <= pane->term->sb->rows)
			drawCursor(pane, line, caretrow, pane->term->cx, pane->term->ctype, now);
	}

	pane->d.redraw_flag = false;

	return 1;
}

void
drawLine(Pane *pane, Line *line, int row, int col, int width, int pos, nsec now)
{
	int next, i = getIndex(line->str, pos);
	int x, y, w;
	int attr, fg, bg, blink, rapid;
	XftColor xc;
	Color fc, bc;
	int sl;

	if (width <= pos || line->str[i] == L'\0')
		return;

	/* 同じ属性の文字はまとめて処理する */
	next = findNextSGR(line, i);
	drawLine(pane, line, row, col, width, pos + u32snwidth(&line->str[i], next - i), now);

	/* 座標 */
	x = pane->d.xpad + (col + pos) * pane->d.xfont->cw;
	y = pane->d.ypad + row * pane->d.xfont->ch;
	w = pane->d.xfont->cw * u32snwidth(&line->str[i], next - i);

	/* 変化無し・コピー・書き直しの分岐 */
#define LINE_CMP(R) linecmp(line, OLD_LINE(pane, R), pos, next - i)
	if (line->attr[i] & (ITALIC | BLINK | RAPID))
		sl = pane->term->sb->rows;
	else if (BETWEEN(row, -1, pane->term->sb->rows + 2) && LINE_CMP(row))
		return;
	else
		for (sl = 0; sl < pane->term->sb->rows; sl++)
			if (LINE_CMP(sl))
				break;
	if (sl < pane->term->sb->rows) {
		XCopyArea(pane->d.dinfo->disp, pane->d.pixbuf, pane->d.pixmap, pane->d.gc,
				x, pane->d.ypad + (sl) * pane->d.xfont->ch,
				w, pane->d.xfont->ch, x, y);
		return;
	}
#undef LINE_CMP

	/* 前処理 */
	fg = line->attr[i] & NEGA ? line->bg[i] : line->fg[i];  /* 反転 */
	bg = line->attr[i] & NEGA ? line->fg[i] : line->bg[i];
	if (line->attr[i] & BOLD)                               /* 太字 */
		fg += fg < 8 ? 8 : 0;
	fc = fg < PALETTE_SIZE ? pane->d.palette[fg] : fg;  /* 色を取得 */
	bc = bg < PALETTE_SIZE ? pane->d.palette[bg] : bg;
	if (line->attr[i] & FAINT)                              /* 細字 */
		fc = BLEND_COLOR(fc, 0.6, bc, 0.4);

	/* 背景を塗る */
	XSetForeground(pane->d.dinfo->disp, pane->d.gc, BELLCOLOR(bc));
	XFillRectangle(pane->d.dinfo->disp, pane->d.pixmap, pane->d.gc, x, y, w, pane->d.xfont->ch);

	/* 非表示・点滅 */
	pane->d.timer_active[BLINK_TIMER] |= line->attr[i] & BLINK;
	pane->d.timer_active[RAPID_TIMER] |= line->attr[i] & RAPID;
	blink = line->attr[i] & BLINK ? ((now / blink_duration) % 2) ? 2 : 0 : 1;
	rapid = line->attr[i] & RAPID ? ((now / rapid_duration) % 2) ? 2 : 0 : 1;
	if (line->attr[i] & CONCEAL || blink + rapid < 2)
		return;

	y += pane->d.xfont->ascent;

	/* 色をXftColorに変換 */
	xc.color.red   =   RED(fc) << 8;
	xc.color.green = GREEN(fc) << 8;
	xc.color.blue  =  BLUE(fc) << 8;
	xc.color.alpha = 0xffff;

	/* 文字を書く */
	attr = FONT_NONE;
	attr |= line->attr[i] & BOLD   ? FONT_BOLD   : FONT_NONE;
	attr |= line->attr[i] & ITALIC ? FONT_ITALIC : FONT_NONE;
	drawXFontString(pane->d.draw, &xc, pane->d.xfont, attr, x, y, w + pane->d.xfont->cw,
			&line->str[i], next - i);

	/* 後処理 */
	XSetForeground(pane->d.dinfo->disp, pane->d.gc, fc);
	if (line->attr[i] & (ULINE | DULINE))   /* 下線 */
		XDrawLine(pane->d.dinfo->disp, pane->d.pixmap, pane->d.gc, x, y + 1, x + w - 1, y + 1);
	if (line->attr[i] & DULINE)             /* 二重下線 */
		XDrawLine(pane->d.dinfo->disp, pane->d.pixmap, pane->d.gc, x, y + 3, x + w - 1, y + 3);
	y -= pane->d.xfont->ascent * 0.4;         /* 取消 */
	if (line->attr[i] & STRIKE)
		XDrawLine(pane->d.dinfo->disp, pane->d.pixmap, pane->d.gc, x, y + 1, x + w - 1, y + 1);
}

void
drawCursor(Pane *pane, Line *line, int row, int col, int type, nsec now)
{
	int index, col2, width;
	getCharCnt(line->str, col, &index, &col2, &width);
	char32_t *c = index < u32slen(line->str) ? &line->str[index] : (char32_t *)L" ";
	const int x = pane->d.xpad + col * pane->d.xfont->cw;
	const int y = pane->d.ypad + row * pane->d.xfont->ch;
	const int cw = pane->d.xfont->cw * width - 1;
	const int ch = pane->d.xfont->ch;
	const DispInfo *dinfo = pane->d.dinfo;
	int attr;
	Line cursor;

	/* 点滅 */
	if ((!type || type % 2) && pane->d.focus &&
	    ((now - pane->d.caret_time) / caret_duration) % 2)
		return;

	XSetForeground(dinfo->disp, pane->d.gc, BELLCOLOR(pane->d.palette[deffg]));

	switch (type) {
	default: case 0: case 1: case 2: /* ブロック */
		if (pane->d.focus) {
			attr = index < u32slen(line->str) ? line->attr[index] : 0;
			cursor = (Line){c, &attr, &defbg, &deffg};
			drawLine(pane, &cursor, row, col2, 1, 0, now);
		} else {
			XDrawRectangle(dinfo->disp, pane->d.pixmap, pane->d.gc, x, y, cw, ch - 1);
			XDrawPoint(dinfo->disp, pane->d.pixmap, pane->d.gc, x + cw, y + ch - 1);
		}
		break;
	case 3: case 4: /* 下線 */
		XFillRectangle(dinfo->disp, pane->d.pixmap, pane->d.gc,
				x, y + 1 + pane->d.xfont->ascent, cw, ch * 0.1);
		break;
	case 5: case 6: /* 縦線 */
		XFillRectangle(dinfo->disp, pane->d.pixmap, pane->d.gc,
				x - 1, y, ch * 0.1, ch);
		break;
	}

	/* 次回の消去範囲を変更 */
	pane->d.clear_x = pane->d.xpad + pane->d.xfont->cw * (col2 - 0.5);
	pane->d.clear_w = cw + pane->d.xfont->cw;
}

void
freePixmap(Pane *pane)
{
	XftDrawDestroy(pane->d.draw);
	XFreeGC(pane->d.dinfo->disp, pane->d.gc);
	XFreePixmap(pane->d.dinfo->disp, pane->d.pixmap);
	XFreePixmap(pane->d.dinfo->disp, pane->d.pixbuf);
}

void
createPixmap(Pane *pane, int w, int h)
{
	const DispInfo *i = pane->d.dinfo;

	pane->d.pixmap = XCreatePixmap(i->disp, i->root, w, h, pane->d.depth);
	pane->d.pixbuf = XCreatePixmap(i->disp, i->root, w, h, pane->d.depth);
	pane->d.gc = XCreateGC(i->disp, pane->d.pixmap, 0, NULL);
	XSetGraphicsExposures(i->disp, pane->d.gc, false);
	pane->d.draw = XftDrawCreate(i->disp, pane->d.pixmap, i->visual, i->cmap);
}

void
clearPixmap(Pane *pane, nsec now)
{
	Line **plines;
	int i;

	/* Pixmapを背景色でクリア */
	XSetForeground(pane->d.dinfo->disp, pane->d.gc, BELLCOLOR(pane->d.palette[defbg]));
	XFillRectangle(pane->d.dinfo->disp, pane->d.pixmap, pane->d.gc, 0, 0, pane->d.width, pane->d.height);
	XSetForeground(pane->d.dinfo->disp, pane->d.gc, BELLCOLOR(pane->d.palette[defbg]));
	XFillRectangle(pane->d.dinfo->disp, pane->d.pixbuf, pane->d.gc, 0, 0, pane->d.width, pane->d.height);

	/* Lineバッファをクリア */
	if (pane->d.new_lines)
		for (plines = pane->d.new_lines; *plines; plines++)
			freeLine(*plines);
	if (pane->d.old_lines)
		for (plines = pane->d.old_lines; *plines; plines++)
			freeLine(*plines);
	pane->d.new_lines = xrealloc(pane->d.new_lines, (pane->term->sb->rows + 4) * sizeof(Line *));
	pane->d.new_lines[pane->term->sb->rows + 3] = NULL;
	pane->d.old_lines = xrealloc(pane->d.old_lines, (pane->term->sb->rows + 4) * sizeof(Line *));
	pane->d.old_lines[pane->term->sb->rows + 3] = NULL;
	for (i = 0; i < pane->term->sb->rows + 3; i++) {
		pane->d.new_lines[i] = allocLine();
		pane->d.old_lines[i] = allocLine();
	}

	pane->d.redraw_flag = true;
}
