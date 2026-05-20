#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

void
errExit(const char *message)
{
	if (message)
		fputs(message, stderr);
	fputs(strerror(errno), stderr);
	fputs("\n", stderr);
	exit(1);
}

void
fatal(const char *message)
{
	if (message)
		fputs(message, stderr);
	exit(1);
}

void *
xmalloc(size_t size)
{
	void *p = malloc(size);
	if (p == NULL)
		errExit("malloc failed.\n");
	return p;
}

void *
xrealloc(void *p, size_t size)
{
	void *p2 = realloc(p, size);
	if (p2 == NULL)
		errExit("realloc failed.\n");
	return p2;
}

/* ini file */

struct Ini {
	struct Pair {
		char key[256];
		char val[256];
	} *pairs;
};

Ini *readIni(const char *dir, const char *filename)
{
	Ini *ini;
	char *conf_home, path[1024], line[1024], key[256], val[256];
	FILE *file;
	int n = 0, len = 8;

	ini = xmalloc(sizeof(Ini));
	ini->pairs = xmalloc(sizeof(struct Pair) * len);

	/* 設定ファイルを開く */
	conf_home = getenv("XDG_CONFIG_HOME");
	if (conf_home)
		snprintf(path, sizeof(path), "%s/%s/%s", conf_home, dir, filename);
	else
		snprintf(path, sizeof(path), "%s/.config/%s/%s", getenv("HOME"), dir, filename);
	if (!(file = fopen(path, "r")))
		return NULL;

	/* 1行ずつ読む */
	while (fgets(line, sizeof(line), file)) {
		key[0] = val[0] = '\0';

		/* # comment */
		if (sscanf(line, " %*[#]%s", key) == 1)
			continue;

		/* key = val */
		if (sscanf(line, " %[^ =] = %[^ \n]", key, val) == 2) {
			strncpy(ini->pairs[n].key, key, sizeof(ini->pairs[n].key));
			strncpy(ini->pairs[n].val, val, sizeof(ini->pairs[n].val));
			n++;
			if (len <= n) {
				len *= 2;
				ini->pairs = xrealloc(ini->pairs, sizeof(struct Pair) * len);
			}
			continue;
		}
	}
	strncpy(ini->pairs[n].key, "", sizeof(ini->pairs[n].key));

	fclose(file);
	return ini;
}

const char *getIniValue(Ini *ini, const char *key)
{
	const size_t size = sizeof(ini->pairs[0].key);
	int i;

	for (i = 0; ini->pairs[i].key[0] != '\0'; i++)
		if (strncmp(ini->pairs[i].key, key, size) == 0)
			return ini->pairs[i].val;
	return "";
}

void destroyIni(Ini *ini)
{
	free(ini->pairs);
	free(ini);
}
