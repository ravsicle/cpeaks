# cpeaks - Twin Peaks Red Room Matrix rain
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS   = -lncurses -lm
PREFIX  ?= /usr/local

OBJS = cpeaks.o stb_impl.o

cpeaks: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

cpeaks.o: cpeaks.c redroom_png.h vendor/stb_image.h vendor/stb_image_write.h vendor/font8x8_basic.h
	$(CC) $(CFLAGS) -c cpeaks.c -o $@

stb_impl.o: stb_impl.c vendor/stb_image.h vendor/stb_image_write.h
	$(CC) -O2 -w -c stb_impl.c -o $@

install: cpeaks
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 cpeaks $(DESTDIR)$(PREFIX)/bin/cpeaks

clean:
	rm -f cpeaks $(OBJS)

.PHONY: install clean
