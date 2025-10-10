.POSIX:

CFLAGS  = -Wall -D_XOPEN_SOURCE=600 -I/usr/include/freetype2
SRCS    = main.c term.c line.c font.c util.c
OBJS    = $(SRCS:.c=.o)

chitan: $(OBJS)
	$(CC) -o chitan $(OBJS) -lX11 -lXft -lfontconfig

.c.o:
	$(CC) $(CFLAGS) -c $<

main.o: util.h line.h term.h font.h
term.o: util.h line.h term.h
line.o: util.h line.h
font.o: util.h font.h
util.o: util.h

clean:
	rm chitan $(OBJS)

release: CC += -O3 -s
release: clean chitan

install: release
	sudo install chitan /usr/local/bin
	tic -x chitan.info

uninstall:
	rm /usr/local/bin/chitan

.PHONY: clean release install uninstall
