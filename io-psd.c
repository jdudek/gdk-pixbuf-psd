/* -*- mode: C; c-file-style: "linux" -*- */
/* GdkPixbuf library - PSD image loader
 *
 * Copyright (C) 2008 Jan Dudek
 *
 * Authors: Jan Dudek <jd@jandudek.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <stdlib.h>
#include <stdio.h>
#include <gdk-pixbuf/gdk-pixbuf-io.h>
#include <glib/gstdio.h>


typedef struct
{
	guchar  signature[4];  /* file ID, always "8BPS" */
	guint16 version;       /* version number, always 1 */
	guchar  reserved[6];
	guint8  channels;      /* number of color channels (1-24) */
	guint32 rows;          /* height of image in pixels (1-30000) */
	guint32 columns;       /* width of image in pixels (1-30000) */
	guint16 depth;         /* number of bits per channel (1, 8, and 16) */
	guint16 color_mode;    /* color mode as defined below */
} PsdHeader;

typedef enum
{
	PSD_MODE_MONO = 0,
	PSD_MODE_GRAYSCALE = 1,
	PSD_MODE_INDEXED = 2,
	PSD_MODE_RGB = 3,
	PSD_MODE_CMYK = 4,
	PSD_MODE_MULTICHANNEL = 7,
	PSD_MODE_DUOTONE = 8,
	PSD_MODE_LAB = 9,
} PsdColorMode;

typedef enum
{
	PSD_COMPRESSION_NONE = 0,
	PSD_COMPRESSION_RLE = 1
} PsdCompressionType;

static guint16
read_uint16 (FILE *fp)
{
	guint16 t;
	t = fgetc(fp) << 8;
	t |= fgetc(fp);
	return t;
}

static guint32
read_uint32 (FILE *fp)
{
	guint32 t;
	t = fgetc(fp) << 24;
	t |= fgetc(fp) << 16;
	t |= fgetc(fp) << 8;
	t |= fgetc(fp);
	return t;
}

static PsdHeader
psd_read_header (FILE *fp)
{
	PsdHeader hd;
	int t;

	fread(hd.signature, 1, 4, fp);
	hd.version = read_uint16(fp);
	fread(hd.reserved, 1, 6, fp);
	hd.channels = read_uint16(fp);
	hd.rows = read_uint32(fp);
	hd.columns = read_uint32(fp);
	hd.depth = read_uint16(fp);
	hd.color_mode = read_uint16(fp);

	// skip Color Mode Data Block
	t = read_uint32(fp);
	fseek(fp, t, SEEK_CUR);
	
	// skip Image Resources Block
	t = read_uint32(fp);
	fseek(fp, t, SEEK_CUR);
	
	// skip Layer and Mask Information Block
	t = read_uint32(fp);
	fseek(fp, t, SEEK_CUR);

	return hd;
}

static GdkPixbuf*
gdk_pixbuf__psd_image_load (FILE *fp,
                            GError **error)
{
	guint rowstride;
	guint16 compression_type;
	guchar *pixels;
	GdkPixbuf *pixbuf;
	guchar **buffers;

	PsdHeader hd = psd_read_header(fp);
	pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, hd.columns, hd.rows);

	if (pixbuf == NULL) {
		g_set_error (error, GDK_PIXBUF_ERROR,
			GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY,
			("Insufficient memory to load PSD image file"));
		return NULL;
	}
	
	pixels = gdk_pixbuf_get_pixels (pixbuf);
	rowstride = gdk_pixbuf_get_rowstride (pixbuf);
	
	compression_type = read_uint16(fp);
	
	if (compression_type != PSD_COMPRESSION_NONE &&
	    compression_type != PSD_COMPRESSION_RLE) {
		g_set_error (error, GDK_PIXBUF_ERROR,
			GDK_PIXBUF_ERROR_UNKNOWN_TYPE,
			("Unsupported compression type"));
		return NULL;
	}
	
	if (hd.color_mode != PSD_MODE_RGB) {
		g_set_error (error, GDK_PIXBUF_ERROR,
			GDK_PIXBUF_ERROR_UNKNOWN_TYPE,
			("Unsupported color mode"));
		return NULL;
	}
	
	//g_message("mode=%d, channels=%d", hd.color_mode, hd.channels);
	
	buffers = g_malloc(sizeof(guchar*) * hd.channels);
	
	if (compression_type == PSD_COMPRESSION_RLE) {
		guint16 *line_lengths = g_malloc(2 * hd.rows * hd.channels);
		
		for (int i = 0; i < hd.rows * hd.channels; ++i) {
			line_lengths[i] = read_uint16(fp);
		}
		
		for (int i = 0; i < hd.channels; ++i) {
			buffers[i] = g_malloc(hd.rows * hd.columns);
			gint position = 0;

			for (int j = 0; j < hd.rows; ++j) {
				guint16 bytes_read = 0;
				while (bytes_read < line_lengths[i * hd.rows + j]) {
					gchar byte = fgetc(fp);
					++bytes_read;
					
					if (byte == -128) {
						continue;
					} else if (byte > -1) {
						gint count = byte + 1;
						
						// copy next count bytes
						for (int k = 0; k < count; ++k) {
							buffers[i][position] = fgetc(fp);
							++bytes_read;
							++position;
						}
					} else {
						gint count = -byte + 1;
						
						// copy next byte count times
						guchar next_byte = fgetc(fp);
						++bytes_read; 
						for (int k = 0; k < count; ++k) {
							buffers[i][position] = next_byte;
							++position;
						}
					}
				}
			}
		}
		
		g_free(line_lengths);
	}
	
	if (hd.color_mode == PSD_MODE_RGB) {
		for (int i = 0; i < hd.rows; ++i) {
			for (int j = 0; j < hd.columns; ++j) {
				pixels[i * rowstride + 3 * j + 0] = buffers[0][i * hd.columns + j];
				pixels[i * rowstride + 3 * j + 1] = buffers[1][i * hd.columns + j];
				pixels[i * rowstride + 3 * j + 2] = buffers[2][i * hd.columns + j];
			}
		}
	}
	// TODO: other color modes, CMYK at least
	
	return pixbuf;
}

void
fill_vtable (GdkPixbufModule* module)
{
/*	module->load = gdk_pixbuf__xbm_image_load;
	module->begin_load = gdk_pixbuf__xbm_image_begin_load;
	module->stop_load = gdk_pixbuf__xbm_image_stop_loads
	module->load_increment = gdk_pixbuf__xbm_image_load_increment;*/
	// TODO progressive loading

	module->load = gdk_pixbuf__psd_image_load;
}

void
fill_info (GdkPixbufFormat *info)
{
	static GdkPixbufModulePattern signature[] = {
		{ "8BPS", NULL, 100 },
		{ NULL, NULL, 0 }
	};
	static gchar * mime_types[] = {
		"image/x-psd",
		NULL
	};
	static gchar * extensions[] = {
		"psd",
		NULL
	};

	info->name = "psd";
	info->signature = signature;
	//info->description = N_("Adobe Photoshop format");
	info->description = "Adobe Photoshop format";
	info->mime_types = mime_types;
	info->extensions = extensions;
	info->flags = GDK_PIXBUF_FORMAT_THREADSAFE;
	info->flags = 0;
	info->license = "LGPL";
}

