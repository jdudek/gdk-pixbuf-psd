CC = gcc
CFLAGS = -Wall -std=c99

DESTDIR=

all:
	$(CC) $(CFLAGS) io-psd.c -o libpixbufloader-psd.so \
		`pkg-config --cflags gtk+-2.0` \
		`pkg-config --libs gtk+-2.0` \
		-shared -fpic -DGDK_PIXBUF_ENABLE_BACKEND

clean:
	rm libpixbufloader-psd.so

install:
	chmod 644 libpixbufloader-psd.so
	mkdir -p $(DESTDIR)/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders/
	cp libpixbufloader-psd.so $(DESTDIR)/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders/

