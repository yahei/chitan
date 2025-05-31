#include <sys/types.h>

typedef struct Term Term;
typedef struct Line Line;

/* Term */
Term *newTerm(void);
void deleteTerm(Term *);
int getfdTerm(Term *);
ssize_t readpty(Term *);
int getlastlineTerm(Term *);
Line *getlineTerm(Term *, int);

/* Line */
Line *newLine(void);
void deleteLine(Line *);
void setmbLine(Line *, char *, int);
char *getmbLine(Line *);
void overwritembLine(Line *, char *, int, int);
