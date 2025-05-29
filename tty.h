typedef struct Term Term;
typedef struct Line Line;

/* Term */
Term *newTerm(void);
void deleteTerm(Term *);
int getfdTerm(Term *);
void readpty(Term *);
int getlastlineTerm(Term *);
Line *getlineTerm(Term *, int);

/* Line */
Line *newLine(void);
void deleteLine(Line *);
void setmbLine(Line *, char *);
char *getmbLine(Line *);
