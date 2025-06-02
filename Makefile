.POSIX:
.PHONY: clean

minty: x.o tty.o util.o
	$(CC) -Wall -o minty x.o tty.o util.o -lX11 -lXft -lfontconfig

x.o: x.c tty.h util.h
	$(CC) -Wall -c x.c -I /usr/include/freetype2

tty.o: tty.c tty.h util.h
	$(CC) -Wall -c tty.c

util.o: util.c util.h
	$(CC) -Wall -c util.c

clean:
	rm minty x.o tty.o util.o
