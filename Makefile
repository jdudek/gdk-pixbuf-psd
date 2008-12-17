# ok, our Makefile slightly sucks at the moment

CC = gcc
CFLAGS=-Wall -std=c99

DESTDIR=

all:
	$(CC) $(CFLAGS) io-psd.c  -o libpixbufloader-psd.so \
		`pkg-config --cflags gtk+-2.0` \
		-shared -fpic -DGDK_PIXBUF_ENABLE_BACKEND

clean:

install:
	chmod 644 libpixbufloader-psd.so
	mkdir -p $(DESTDIR)/usr/lib/gtk-2.0/2.10.0/loaders/
	cp libpixbufloader-psd.so $(DESTDIR)/usr/lib/gtk-2.0/2.10.0/loaders/

# and then run the following as root:
# gdk-pixbuf-query-loaders libpixbufloader-psd.so >> /etc/gtk-2.0/gdk-pixbuf.loaders


