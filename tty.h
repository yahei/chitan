/* Term */
typedef struct Term Term;

Term *newTerm(void);
void deleteTerm(Term *);

/* Line */
typedef struct Line Line;

Line *newLine(void);
void deleteLine(Line *);
void setmbLine(Line *, char *);
char *getmbLine(Line *);
