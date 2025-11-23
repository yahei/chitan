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
#define BELLCOLOR(c)    (now < pane->bell_time ? BLEND_COLOR((c), 0.925, 0xffffffff, 0.075) : (c))
#define SCROLLMAX(sb)   ((sb)->firstline - MAX((sb)->totallines - (sb)->maxlines, 0))
#define NEW_LINE(p, n)  (pane->new_lines[n + 1])
#define OLD_LINE(p, n)  (pane->old_lines[n + 1])
const long long blink_duration = 800 * 1000 * 1000;
const long long rapid_duration = 200 * 1000 * 1000;
const long long caret_duration = 500 * 1000 * 1000;

static void drawLine(Pane *, Line *, int, int, int, int, nsec);
static void drawCursor(Pane *, Line *, int, int, int, nsec);
static void freePixmap(Pane *);
static void createPixmap(Pane *, int, int);
static void clearPixmap(Pane *, nsec);

Pane *
createPane(DispInfo *dinfo, XFont *xfont, int width, int height, float alpha, int bufsize, char *const cmd[])
{
	char *xrm, *str_type, buf[16];
	XrmDatabase xdb;
	XrmValue val;
	int i;
	Pane *pane = xmalloc(sizeof(Pane));

	*pane = (Pane){
		.dinfo = dinfo, .xfont = xfont, .depth = 32,
		.width = width, .height = height,
		.xpad = xfont->cw / 2, .ypad = xfont->cw / 2,
	};
	memset(&pane->timer_active, 0, TIMER_NUM);

	/* 端末をオープン */
	pane->term = openTerm((height - pane->ypad * 2) / xfont->ch,
			(width - pane->xpad * 2) / xfont->cw, bufsize, cmd[0], cmd);
	if (!pane->term)
		errExit("openTerm failed.\n");

	/* パレットの設定 */
	xrm = XResourceManagerString(dinfo->disp);
	xdb = XrmGetStringDatabase(xrm ? xrm : "");
#define XRCOLOR(name, num) do {\
	if (XrmGetResource(xdb, (name), "chitan", &str_type, &val) &&\
	    strncmp(str_type, "String", 6) == 0 &&\
	    strlen(val.addr) ==7 && val.addr[0] == '#')\
		pane->term->palette[num] = strtol(val.addr + 1, NULL, 16) + 0xff000000;\
} while (0)
	XRCOLOR("chitan.foreground", deffg);
	XRCOLOR("chitan.background", defbg);
	for (i = 0; i < 256; i++) {
		snprintf(buf, 16, "chitan.color%d", i);
		XRCOLOR(buf, i);
	}
#undef XRCOLOR
	XrmDestroyDatabase(xdb);
	pane->term->palette[defbg] = ((0xff & (int)(0xff * alpha)) << 24) +
		(0x00ffffff &pane->term->palette[defbg]);
	for (i = 0; i < PALETTE_SIZE; i++)
		pane->term->def_palette[i] = pane->term->palette[i];

	/* 描画の準備 */
	createPixmap(pane, width, height);
	clearPixmap(pane, pane->time_b);

	return pane;
}

void
destroyPane(Pane *pane)
{
	Line **plines;

	closeTerm(pane->term);
	freePixmap(pane);
	for (plines = pane->new_lines; *plines; plines++)
		freeLine(*plines);
	free(pane->new_lines);
	for (plines = pane->old_lines; *plines; plines++)
		freeLine(*plines);
	free(pane->old_lines);
	free(pane);
}

void
setPaneSize(Pane *pane, int width, int height)
{
	pane->width = width;
	pane->height = height;
	setWinSize(pane->term, (height - pane->ypad * 2) / pane->xfont->ch,
			(width - pane->xpad * 2) / pane->xfont->cw, width, height);
	freePixmap(pane);
	createPixmap(pane, width, height);
	clearPixmap(pane, pane->time_b);
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
	pane->scr = CLIP(pane->scr + n, 0, SCROLLMAX(pane->term->sb));
}

void
selectPane(Pane *pane, int row, int col, bool start, bool rect)
{
	pane->redraw_flag = true;
	setSelection(&pane->sel, pane->term->sb, row - pane->scr, col, start, rect);
}

nsec
getNextTime(Pane *pane, nsec now)
{
	nsec time = (nsec)2 << 32;

	/* ベルの時間 */
	if (now < pane->bell_time)
		time = pane->bell_time - now;

#define wait(t, d)      ((d) - (now - (t)) % (d))
	/* 点滅の時刻 */
	if (pane->timer_active[BLINK_TIMER])
		time = MIN(wait(0, blink_duration), time);
	if (pane->timer_active[RAPID_TIMER])
		time = MIN(wait(0, rapid_duration), time);
	if (pane->timer_active[CARET_TIMER])
		time = MIN(wait(pane->caret_time, caret_duration), time);
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

	/* --- タイマーの処理 --- */

	/* 点滅させる必要がないときはキャレットのタイマーを止める */
	pane->timer_active[CARET_TIMER] = pane->focus &&
		(pane->term->cy + pane->scr <= pane->term->sb->rows) &&
		(!pane->term->ctype || pane->term->ctype % 2);

	/* ベルの消灯時刻をまたいでいたら画面クリア */
	if (pane->time_b < pane->bell_time && pane->bell_time <= now)
		pane->redraw_flag = clear_flag = true;

	/* 点滅の切り替わり時刻をまたいでいたら再描画 */
#define LIT(T,D) (((T) / (D)) % 2)
#define CHECK(T,D,B) (pane->timer_active[T] && LIT(pane->time_b - (B), D) != LIT( now - (B), D))
	if (CHECK(BLINK_TIMER, blink_duration, 0) ||
	    CHECK(RAPID_TIMER, rapid_duration, 0) ||
	    CHECK(CARET_TIMER, caret_duration, pane->caret_time))
		pane->redraw_flag = true;
#undef CHECK
#undef LIT

	pane->time_b = now;

	if (!pane->redraw_flag)
		return 0;

	/* --- 描画前の処理 --- */

	/* ベルやパレットの更新をチェック */
	if (pane->bell_cnt != pane->term->bell_cnt) {
		clear_flag |= pane->bell_time <= now;
		pane->bell_time = now + bell_duration;
		pane->bell_cnt = pane->term->bell_cnt;
	}
	if (pane->palette_cnt != pane->term->palette_cnt) {
		clear_flag = true;
		pane->palette_cnt = pane->term->palette_cnt;
	}

	/* バッファの切り替えや行の追加を見てスクロール量を更新 */
	pane->scr = (pane->prevbuf != pane->term->sb) ? 0 : pane->scr;
	pane->scr += (0 < pane->scr) ? pane->term->sb->firstline - pane->prevfst : 0;
	pane->scr = CLIP(pane->scr, 0, SCROLLMAX(pane->term->sb));
	pane->prevbuf = pane->term->sb;
	pane->prevfst = pane->term->sb->firstline;

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
		XCopyArea(pane->dinfo->disp, pane->pixbuf, pane->pixmap, pane->gc,
				pane->clear_x, pane->clear_y,
				pane->clear_w, pane->clear_h,
				pane->clear_x, pane->clear_y);

	/* 次回の消去範囲を設定 */
	pane->clear_x = pane->xpad + pane->xfont->cw * (pane->term->cx - 0.5);
	pane->clear_y = pane->ypad + pane->xfont->ch * (pane->term->cy + pane->scr);
	pane->clear_w = pane->xfont->cw * 2;
	pane->clear_h = pane->xfont->ch;

	/* 端末の内容を取得 */
	getLines(pane->term->sb, pane->new_lines, pane->term->sb->rows + 3,
			pane->scr + 1, &pane->sel);

	/* -1行目は画面端をまたいで選択してる場合だけ書く */
	if ((pane->sel.aline < pane->term->sb->firstline - pane->scr) ==
	    (pane->sel.bline < pane->term->sb->firstline - pane->scr) ||
	     pane->term->sb != pane->sel.sb)
		PUT_NUL(NEW_LINE(pane, -1), 0);

	/* 点滅中フラグを一旦クリア */
	pane->timer_active[BLINK_TIMER] = pane->timer_active[RAPID_TIMER] = false;

	/* Pixmapに書く */
	for (i = -1; i < pane->term->sb->rows + 2; i++) {
		line = NEW_LINE(pane, i);

		/* 前回の方が長い場合の塗りつぶし */
		width   = line ? u32swidth(line->str) : 0;
		width_b = u32swidth(OLD_LINE(pane, i)->str) + 1;
		if (width < width_b) {
			XSetForeground(pane->dinfo->disp, pane->gc,
					BELLCOLOR(pane->term->palette[defbg]));
			XFillRectangle(pane->dinfo->disp, pane->pixmap, pane->gc,
					pane->xpad + pane->xfont->cw * width,
					pane->ypad + pane->xfont->ch * i,
					pane->xfont->cw * (width_b - width),
					pane->xfont->ch);
		}

		/* 行を書く */
		if (line)
			drawLine(pane, line, i, 0, pane->term->sb->cols + 2, 0, now);
	}

	/* 書いた文字とPixmapの状態を記録 */
	for (i = -1; i < pane->term->sb->rows + 2; i++)
		linecpy(OLD_LINE(pane, i), NEW_LINE(pane, i));
	XCopyArea(pane->dinfo->disp, pane->pixmap, pane->pixbuf, pane->gc,
			0, 0, pane->width, pane->height, 0, 0);

	/* --- カーソル/Preeditの描画 --- */

	XSetForeground(pane->dinfo->disp, pane->gc, pane->term->palette[deffg]);
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
		pane->clear_x = pane->xpad + pane->xfont->cw * (pepos - 0.5);
		pane->clear_w = pane->xfont->cw * (pewidth + 1);
	} else if (1 <= pane->term->dec[25] && pane->term->cx < pane->term->sb->cols + 2) {
		/* カーソルの描画 */
		caretrow = pane->term->cy + pane->scr;
		line = NEW_LINE(pane, caretrow);
		if (caretrow <= pane->term->sb->rows)
			drawCursor(pane, line, caretrow, pane->term->cx, pane->term->ctype, now);
	}

	pane->redraw_flag = false;

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
	x = pane->xpad + (col + pos) * pane->xfont->cw;
	y = pane->ypad + row * pane->xfont->ch;
	w = pane->xfont->cw * u32snwidth(&line->str[i], next - i);

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
	fc = fg < PALETTE_SIZE ? pane->term->palette[fg] : fg;  /* 色を取得 */
	bc = bg < PALETTE_SIZE ? pane->term->palette[bg] : bg;
	if (line->attr[i] & FAINT)                              /* 細字 */
		fc = BLEND_COLOR(fc, 0.6, bc, 0.4);

	/* 背景を塗る */
	XSetForeground(pane->dinfo->disp, pane->gc, BELLCOLOR(bc));
	XFillRectangle(pane->dinfo->disp, pane->pixmap, pane->gc, x, y, w, pane->xfont->ch);

	/* 非表示・点滅 */
	pane->timer_active[BLINK_TIMER] |= line->attr[i] & BLINK;
	pane->timer_active[RAPID_TIMER] |= line->attr[i] & RAPID;
	blink = line->attr[i] & BLINK ? ((now / blink_duration) % 2) ? 2 : 0 : 1;
	rapid = line->attr[i] & RAPID ? ((now / rapid_duration) % 2) ? 2 : 0 : 1;
	if (line->attr[i] & CONCEAL || blink + rapid < 2)
		return;

	y += pane->xfont->ascent;

	/* 色をXftColorに変換 */
	xc.color.red   =   RED(fc) << 8;
	xc.color.green = GREEN(fc) << 8;
	xc.color.blue  =  BLUE(fc) << 8;
	xc.color.alpha = 0xffff;

	/* 文字を書く */
	attr = FONT_NONE;
	attr |= line->attr[i] & BOLD   ? FONT_BOLD   : FONT_NONE;
	attr |= line->attr[i] & ITALIC ? FONT_ITALIC : FONT_NONE;
	drawXFontString(pane->draw, &xc, pane->xfont, attr, x, y, w + pane->xfont->cw,
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

void
drawCursor(Pane *pane, Line *line, int row, int col, int type, nsec now)
{
	const CharCnt cc = getCharCnt(line->str, col);
	char32_t *c = cc.index < u32slen(line->str) ? &line->str[cc.index] : (char32_t *)L" ";
	const int x = pane->xpad + col * pane->xfont->cw;
	const int y = pane->ypad + row * pane->xfont->ch;
	const int cw = pane->xfont->cw * u32snwidth(c, 1) - 1;
	const int ch = pane->xfont->ch;
	const DispInfo *dinfo = pane->dinfo;
	int attr;
	Line cursor;

	/* 点滅 */
	if (((now - pane->caret_time) / caret_duration) % 2 &&
			pane->focus && (!type || type % 2))
		return;

	XSetForeground(dinfo->disp, pane->gc, BELLCOLOR(pane->term->palette[deffg]));

	switch (type) {
	default: case 0: case 1: case 2: /* ブロック */
		if (pane->focus) {
			attr = cc.index < u32slen(line->str) ? line->attr[cc.index] : 0;
			cursor = (Line){c, &attr, &defbg, &deffg};
			drawLine(pane, &cursor, row, cc.col, 1, 0, now);
		} else {
			XDrawRectangle(dinfo->disp, pane->pixmap, pane->gc, x, y, cw, ch - 1);
			XDrawPoint(dinfo->disp, pane->pixmap, pane->gc, x + cw, y + ch - 1);
		}
		break;
	case 3: case 4: /* 下線 */
		XFillRectangle(dinfo->disp, pane->pixmap, pane->gc,
				x, y + 1 + pane->xfont->ascent, cw, ch * 0.1);
		break;
	case 5: case 6: /* 縦線 */
		XFillRectangle(dinfo->disp, pane->pixmap, pane->gc,
				x - 1, y, ch * 0.1, ch);
		break;
	}

	/* 次回の消去範囲を変更 */
	pane->clear_x = pane->xpad + pane->xfont->cw * (cc.col - 0.5);
	pane->clear_w = cw + pane->xfont->cw;
}

void
freePixmap(Pane *pane)
{
	XftDrawDestroy(pane->draw);
	XFreeGC(pane->dinfo->disp, pane->gc);
	XFreePixmap(pane->dinfo->disp, pane->pixmap);
	XFreePixmap(pane->dinfo->disp, pane->pixbuf);
}

void
createPixmap(Pane *pane, int w, int h)
{
	const DispInfo *i = pane->dinfo;

	pane->pixmap = XCreatePixmap(i->disp, i->root, w, h, pane->depth);
	pane->pixbuf = XCreatePixmap(i->disp, i->root, w, h, pane->depth);
	pane->gc = XCreateGC(i->disp, pane->pixmap, 0, NULL);
	XSetGraphicsExposures(i->disp, pane->gc, false);
	pane->draw = XftDrawCreate(i->disp, pane->pixmap, i->visual, i->cmap);
}

void
clearPixmap(Pane *pane, nsec now)
{
	Line **plines;
	int i;

	/* Pixmapを背景色でクリア */
	XSetForeground(pane->dinfo->disp, pane->gc, BELLCOLOR(pane->term->palette[defbg]));
	XFillRectangle(pane->dinfo->disp, pane->pixmap, pane->gc, 0, 0, pane->width, pane->height);
	XSetForeground(pane->dinfo->disp, pane->gc, BELLCOLOR(pane->term->palette[defbg]));
	XFillRectangle(pane->dinfo->disp, pane->pixbuf, pane->gc, 0, 0, pane->width, pane->height);

	/* Lineバッファをクリア */
	if (pane->new_lines)
		for (plines = pane->new_lines; *plines; plines++)
			freeLine(*plines);
	if (pane->old_lines)
		for (plines = pane->old_lines; *plines; plines++)
			freeLine(*plines);
	pane->new_lines = xrealloc(pane->new_lines, (pane->term->sb->rows + 4) * sizeof(Line *));
	pane->new_lines[pane->term->sb->rows + 3] = NULL;
	pane->old_lines = xrealloc(pane->old_lines, (pane->term->sb->rows + 4) * sizeof(Line *));
	pane->old_lines[pane->term->sb->rows + 3] = NULL;
	for (i = 0; i < pane->term->sb->rows + 3; i++) {
		pane->new_lines[i] = allocLine();
		pane->old_lines[i] = allocLine();
	}

	pane->redraw_flag = true;
}
