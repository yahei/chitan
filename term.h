#include <stdbool.h>
#include <unistd.h>

#include "line.h"

#define ALPHA(c)        ((c) >> 24 & 0xff)
#define RED(c)          ((c) >> 16 & 0xff)
#define GREEN(c)        ((c) >>  8 & 0xff)
#define BLUE(c)         ((c) >>  0 & 0xff)
#define DECMODE(term,n) ((term)->decmode[n] % 2)

#define TITLE_MAX       (256)
#define READ_SIZE       (1 << 14)

enum mouse_event_type {
	SHIFT   = 4,
	ALT     = 8,
	CTRL    = 16,
	MOVE    = 32,
	WHEEL   = 64,
	OTHER   = 128
};

/* バッファ */
typedef struct ScrBuf {
	Line **lines;   /* バッファ */
	int maxlines;   /* バッファの最大行数*/
	int firstline;  /* 画面上1行目となる行 */
	int totallines; /* バッファの総行数 */
	int rows, cols; /* 画面の行数と列数 */
	int scrs, scre; /* スクロール範囲 */
	int am;         /* 自動改行 */
} ScrBuf;

/* 選択範囲 */
typedef struct Selection {
	struct ScrBuf *sb;
	int aline, acol, bline, bcol;
	int rect;
	int *vers;
} Selection;

/* 受信モード */
enum receive_mode {
	RCV_NORMAL,
	RCV_DCS,
	RCV_SOS,
	RCV_OSC,
	RCV_PM,
	RCV_APC,
};

/* 端末 */
typedef struct Term {
	int master;             /* 疑似端末のFD */
	pid_t pid;              /* 子プロセスのpid */
	ScrBuf ori, alt, *sb;   /* バッファ */
	int cx, cy;             /* カーソル位置 */
	int svx, svy;           /* 保存したカーソル位置 */
	int ctype;              /* カーソル形状 */
	char readbuf[READ_SIZE + 1];    /* リードバッファ */
	int rblen;              /* リードバッファに残っている文字の数 */
	enum receive_mode rcv;  /* 受信モード */
	unsigned int err_cnt;   /* 未対応の表示の文字数カウンタ */
	char mode[64];          /* ANSIモード */
	char decmode[8800];     /* DECプライベートモード */
	char appkeypad;         /* Application Keypadの状態 */
	int attr;               /* 現在の属性 */
	Color fg, bg;           /* 現在の色 */
	Color palette[PALETTE_SIZE];        /* カラーパレット */
	Color def_palette[PALETTE_SIZE];    /* カラーパレットのデフォルト値 */
	int oldmx, oldmy;       /* 前回のマウス座標 */
	const char32_t *g[4];   /* 文字集合 */
	const char32_t **gl;    /* 呼び出されている文字集合 */
	char title[TITLE_MAX];  /* タイトル */
	int bell_cnt;           /* ベルが鳴った回数 */
	int palette_cnt;        /* パレットを変更した回数 */
} Term;

Term *openTerm(int, int, int, const char *, char *const []);
void closeTerm(Term *);
ssize_t readPty(Term *);
ssize_t writePty(Term *, const char *, ssize_t);
void setWinSize(Term *, int, int, int, int);
void reportMouse(Term *, int, int, int, int);

Line *getLine(const ScrBuf *, int);
void getLines(const ScrBuf *, Line **, int, int, const Selection *);

void setSelection(Selection *, ScrBuf *sb, int, int, bool, bool);
bool checkSelection(Selection *);
void copySelection(Selection *, char **, bool);
