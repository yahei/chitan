minty: x.o tty.o util.o
	c99 -o minty x.o tty.o util.o -lX11 -lXft

x.o: x.c tty.h util.h
	c99 -I /usr/include/freetype2 -c x.c

tty.o: tty.c tty.h util.h
	c99 -c tty.c

util.o: util.c util.h
	c99 -c util.c

clean:
	rm minty x.o tty.o util.o
