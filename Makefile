.POSIX:
.PHONY: clean

minty: main.o term.o line.o util.o
	$(CC) -Wall -o minty main.o term.o line.o util.o -lX11 -lXft -lfontconfig

main.o: main.c term.h line.h util.h
	$(CC) -Wall -c main.c -I /usr/include/freetype2

term.o: term.c term.h line.h util.h
	$(CC) -Wall -c term.c

line.o: line.c line.h util.h
	$(CC) -Wall -c line.c

util.o: util.c util.h
	$(CC) -Wall -c util.c

clean:
	rm minty main.o term.o line.o util.o
