#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include "tty.h"

void
openpty(void)
{
	int m,s;
	char *sname;
	char buf[64];

	/* 疑似端末をオープン */
	m = posix_openpt(O_RDWR);

	/* スレーブの名前を取得 */
	sname = ptsname(m);

	if (sname) {
		/* スレーブデバイスの所有権や許可を変更 */
		grantpt(m);
		/* スレーブのロックを解除 */
		unlockpt(m);
		/* スレーブをオープン */
		s = open(sname, O_RDWR);
	}

	/* 表示 */
	printf("%d, %s, %d\n", m, sname, s);

	/* 本体とフォアグラウンドプロセスにフォーク */
	switch (fork()) {
	case 0: /* プロセス側 */
		printf("child.\n");
		/* 標準入出力を繋ぎ変える */
		dup2(s, 0);
		dup2(s, 1);
		dup2(s, 2);
		/* プロセス起動 */
		execl("/usr/bin/echo", "test message.");
		break;
	default: /* 本体側 */
		printf("parent.\n");
		/* すぐ受け取れないとここで待ちっぱなしになる */
		read(m, buf, sizeof(buf));
		printf("%s\n", buf);
	}
}
