SH=/bin/sh
CFLAGS=-Wall -g -O
LDFLAGS=-g

all: gen_part boot0img

gen_part: gen_part.o

boot0img: boot0img.o

.PHONY: clean distclean

clean:
	rm -f *.o

distclean: clean
	rm -f gen_part boot0img

