/* 行 */
typedef struct Line Line;

Line *newLine(void);
void deleteLine(Line *);
void setUtf8(Line *, char *, int);
const char *getUtf8(Line *);
void overwriteUtf8(Line *, char *, int, int);
