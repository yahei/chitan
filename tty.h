#include <sys/types.h>

typedef struct Term Term;
typedef struct Line Line;

/* Term */
Term *newTerm(void);
void deleteTerm(Term *);
int getfdTerm(Term *);
ssize_t readptyTerm(Term *);
ssize_t writeptyTerm(Term *, char *, ssize_t);
int getlastlineTerm(Term *);
Line *getlineTerm(Term *, int);
int getcursorTerm(Term *);

/* Line */
Line *newLine(void);
void deleteLine(Line *);
void setmbLine(Line *, char *, int);
char *getmbLine(Line *);
void overwritembLine(Line *, char *, int, int);
