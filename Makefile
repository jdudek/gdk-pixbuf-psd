# ok, our Makefile slightly sucks at the moment

CC = gcc
CFLAGS=-Wall -std=c99

all:
	$(CC) $(CFLAGS) io-psd.c  -o libpixbufloader-psd.so \
		`pkg-config --cflags gtk+-2.0` \
		-shared -fpic -DGDK_PIXBUF_ENABLE_BACKEND

# and then run the following as root:
# gdk-pixbuf-query-loaders libpixbufloader-psd.so >> /etc/gtk-2.0/gdk-pixbuf.loaders


