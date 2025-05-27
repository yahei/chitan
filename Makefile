minty: x.o tty.o
	c99 -o minty x.o tty.o -lX11 -lXft

x.o: x.c tty.h
	c99 -I /usr/include/freetype2 -c x.c

tty.o: tty.c tty.h
	c99 -c tty.c

clean:
	rm tty.o x.o minty
