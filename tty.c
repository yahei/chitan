#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include "tty.h"

struct Term {
	int master, slave;      /* 疑似端末のファイルディスクリプタ */
};

Term *
openterm(void)
{
	Term *term = malloc(sizeof(Term));
	char *sname;
	char buf[64];

	/* 疑似端末をオープン */
	term->master = posix_openpt(O_RDWR);

	/* スレーブの名前を取得 */
	sname = ptsname(term->master);

	if (sname) {
		/* スレーブデバイスの所有権や許可を変更 */
		grantpt(term->master);
		/* スレーブのロックを解除 */
		unlockpt(term->master);
		/* スレーブをオープン */
		term->slave = open(sname, O_RDWR);
	}

	/* 表示 */
	printf("%d, %s, %d\n", term->master, sname, term->slave);

	/* 本体とフォアグラウンドプロセスにフォーク */
	switch (fork()) {
	case 0: /* プロセス側 */
		close(term->master);
		printf("child.\n");
		/* 標準入出力を繋ぎ変える */
		dup2(term->slave, 0);
		dup2(term->slave, 1);
		dup2(term->slave, 2);
		/* プロセス起動 */
		execl("/usr/bin/echo", "test message.");
		break;
	default: /* 本体側 */
		close(term->slave);
		printf("parent.\n");
		/* すぐ受け取れないとここで待ちっぱなしになる */
		read(term->master, buf, sizeof(buf));
		printf("%s\n", buf);
	}

	return term;
}

void
closeterm(Term *term)
{
	if (term->master < 0) {
		fprintf(stderr, "The term is already closed.\n");
		return;
	}

	close(term->master);
	term->slave  = -1;
	term->master = -1;
	free(term);
}
