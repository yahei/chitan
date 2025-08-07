.POSIX:

CFLAGS  = -Wall -D_XOPEN_SOURCE=600 -I/usr/include/freetype2
SRCS    = main.c term.c line.c util.c
OBJS    = $(SRCS:.c=.o)

chitan: $(OBJS)
	$(CC) -o chitan $(OBJS) -lX11 -lXft -lfontconfig

.c.o:
	$(CC) $(CFLAGS) -c $<

main.o: term.h line.h util.h
term.o: term.h line.h util.h
line.o: line.h util.h
util.o: util.h

clean:
	rm chitan $(OBJS)

.PHONY: clean
