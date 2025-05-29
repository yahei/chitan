/* Term */
typedef struct Term Term;

Term *openterm(void);
void closeterm(Term *);

/* Line */
typedef struct Line Line;

Line *createline(void);
void deleteline(Line *);
void setutf8(Line *, char *);
char *getutf8(Line *);
