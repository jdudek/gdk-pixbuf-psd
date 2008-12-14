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

/*
 * TODO
 * - use http://library.gnome.org/devel/glib/unstable/glib-Byte-Order-Macros.html
 * - report errors from parse_psd_header
 * - other color modes
 * - ...
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <gdk-pixbuf/gdk-pixbuf-io.h>
#include <glib/gstdio.h>


typedef struct
{
	guchar  signature[4];  /* file ID, always "8BPS" */
	guint16 version;       /* version number, always 1 */
	guchar  reserved[6];
	guint16 channels;      /* number of color channels (1-24) */
	guint32 rows;          /* height of image in pixels (1-30000) */
	guint32 columns;       /* width of image in pixels (1-30000) */
	guint16 depth;         /* number of bits per channel (1, 8, and 16) */
	guint16 color_mode;    /* color mode as defined below */
} PsdHeader;

#define PSD_HEADER_SIZE 26

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

typedef enum
{
	PSD_STATE_HEADER,
	PSD_STATE_COLOR_MODE_BLOCK,
	PSD_STATE_RESOURCES_BLOCK,
	PSD_STATE_LAYERS_BLOCK,
	PSD_STATE_COMPRESSION,
	PSD_STATE_LINES_LENGTHS,
	PSD_STATE_CHANNEL_DATA,
	PSD_STATE_DONE
} PsdReadState;

typedef struct
{
	PsdReadState       state;
	
	GdkPixbuf*                  pixbuf;

	GdkPixbufModuleSizeFunc     size_func;
	GdkPixbufModuleUpdatedFunc  updated_func;
	GdkPixbufModulePreparedFunc prepared_func; 
	gpointer                    user_data;

	guchar*            buffer;
	guint              bytes_read;
	guint32            bytes_to_skip;
	gboolean           bytes_to_skip_known;

	//PsdHeader          hd;
	guint32            width;         /* width of image in pixels (1-30000) */
	guint32            height;        /* height of image in pixels (1-30000) */
	guint16            channels;      /* number of color channels (1-24) */
	guint16            depth;         /* number of bits per channel (1, 8, and 16) */
	PsdColorMode       color_mode;
	PsdCompressionType compression;

	guchar**           channels_buffers;
	guint              current_channel;
	guint              current_row;
	guint              position; // ? redundant?
	guint16*           lines_lengths;
} PsdContext;

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

static guint16
parse_uint16 (guchar* buf)
{
	return (buf[0] << 8) | buf[1];
}

static guint32
parse_uint32 (guchar* buf)
{
	return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
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

/*
 * Parse Psdheader from buffer
 * str is expected to be at least PSD_HEADER_SIZE long
 */
static PsdHeader
psd_parse_header (guchar* str)
{
	PsdHeader hd;
	
	memcpy(hd.signature, str, 4);
	hd.version = parse_uint16(str + 4);
	hd.channels = parse_uint16(str + 12);
	hd.rows = parse_uint32(str + 14);
	hd.columns = parse_uint32(str + 18);
	hd.depth = parse_uint16(str + 22);
	hd.color_mode = parse_uint16(str + 24);

	return hd;
}

// -- non-progressive loading --------------------------------------------------

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
	pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, hd.columns, hd.rows);

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
				pixels[i*rowstride + 3*j + 0] = buffers[0][i*hd.columns + j];
				pixels[i*rowstride + 3*j + 1] = buffers[1][i*hd.columns + j];
				pixels[i*rowstride + 3*j + 2] = buffers[2][i*hd.columns + j];
			}
		}
	}
	// TODO: other color modes, CMYK at least
	
	return pixbuf;
}


// --- progressive loading -----------------------------------------------------

// Attempts to read bytes_needed bytes from data and stores them
// in buffer.
// Returns true if there were enough bytes and false otherwise
// (which means we need to call feed_buffer again)

static gboolean
feed_buffer (guchar*        buffer,
             guint*         bytes_read,
             const guchar** data,
             guint*         size,
             guint          bytes_needed)
{
	gint how_many = bytes_needed - *bytes_read;
	if (how_many > *size) {
		how_many = *size;
	}
	memcpy(buffer + *bytes_read, *data, how_many);
	*bytes_read += how_many;
	*data += how_many;
	*size -= how_many;
	return (*bytes_read == bytes_needed);
}

// Attempts to read size of the block and then skip this block.
// Returns true when finishes consuming block data, otherwise false
// (false means we must call skip_block once again)

static gboolean
skip_block (PsdContext* context, const guchar** data, guint* size)
{
	static guint counter;

	if (!context->bytes_to_skip_known) {
		context->bytes_read = 0;
		if (feed_buffer(context->buffer, &context->bytes_read, data, size, 4)) {
			context->bytes_to_skip = parse_uint32(context->buffer);
			context->bytes_to_skip_known = TRUE;
			counter = 0;
		} else {
			return FALSE;
		}
	}
	if (*size < context->bytes_to_skip) {
		*data += *size;
		context->bytes_to_skip -= *size;
		counter += *size;
		*size = 0;
		return FALSE;
	} else {
		counter += context->bytes_to_skip;
		*size -= context->bytes_to_skip;
		*data += context->bytes_to_skip;
		return TRUE;
	}
}

// Decodes RLE-compressed data
static void
decompress_line(const guchar* src, guint line_length, guchar* dest)
{
	guint16 bytes_read = 0;
	while (bytes_read < line_length) {
		gchar byte = src[bytes_read];
		++bytes_read;
	
		if (byte == -128) {
			continue;
		} else if (byte > -1) {
			gint count = byte + 1;
		
			// copy next count bytes
			for (int k = 0; k < count; ++k) {
				*dest = src[bytes_read];
				++dest;
				++bytes_read;
			}
		} else {
			gint count = -byte + 1;
		
			// copy next byte count times
			guchar next_byte = src[bytes_read];
			++bytes_read; 
			for (int k = 0; k < count; ++k) {
				*dest = next_byte;
				++dest;
			}
		}
	}
}

static void
reset_context(PsdContext* ctx)
{
	ctx->bytes_read = 0;
	ctx->bytes_to_skip = 0;
	ctx->bytes_to_skip_known = FALSE;
}

static gpointer
gdk_pixbuf__psd_image_begin_load (GdkPixbufModuleSizeFunc size_func,
                                  GdkPixbufModulePreparedFunc prepared_func,
                                  GdkPixbufModuleUpdatedFunc updated_func,
                                  gpointer user_data,
                                  GError **error)
{
	PsdContext* context = g_malloc(sizeof(PsdContext));
	if (context == NULL) {
		g_set_error (
			error,
			GDK_PIXBUF_ERROR,
			GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY,
			("Not enough memory"));
		return NULL;
	}
	context->size_func = size_func;
	context->prepared_func = prepared_func;
	context->updated_func = updated_func;
	context->user_data = user_data;
	
	context->state = PSD_STATE_HEADER;

	// we'll allocate larger buffer once we know image size
	context->buffer = g_malloc(256);
	reset_context(context);

	//context->bytes_read = 0;
	//context->bytes_to_skip = 0;
	//context->bytes_to_skip_known = FALSE;

	context->channels_buffers = NULL;
	context->current_channel = 0;
	context->current_row = 0;
	context->position = 0;
	context->lines_lengths = NULL;

	return (gpointer) context;
}

static gboolean
gdk_pixbuf__psd_image_stop_load (gpointer context_ptr, GError **error)
{
	PsdContext *ctx = (PsdContext *) context_ptr;
	gboolean retval = TRUE;

	if (ctx->state != PSD_STATE_DONE) {
		g_set_error (
			error,
			GDK_PIXBUF_ERROR,
			GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
			("PSD file was corrupted or incomplete."));
		retval = FALSE;
	} else {
		// convert or copy channel buffers to our GdkPixbuf

		//for (int i = 0; i < ctx->channels; i++) {
		guchar* pixels = gdk_pixbuf_get_pixels(ctx->pixbuf);
		for (int i = 0; i < ctx->height; i++) {
			for (int j = 0; j < ctx->width; j++) {
				pixels[3*j+0] = 0x00;
				pixels[3*j+1] = 0x00;
				pixels[3*j+2] = 0x00;
			}
			pixels += gdk_pixbuf_get_rowstride(ctx->pixbuf);
		}
		
		for (int i = 0; i < 3; i++) {
			guchar* pixels = gdk_pixbuf_get_pixels(ctx->pixbuf);
			for (int j = 0; j < ctx->height; j++) {
				guchar* src_buf = &ctx->channels_buffers[i][ctx->width * j];
				for (int k = 0; k < ctx->width; k++) {
					pixels[3 * k + i] = src_buf[k];
				}
				pixels += gdk_pixbuf_get_rowstride(ctx->pixbuf);
			}
		}
	}
	
	g_free (ctx->buffer); // TODO a few more buffers need freeing
	g_free (ctx);
	
	return retval;
}


static gboolean
gdk_pixbuf__psd_image_load_increment (gpointer      context_ptr,
                                      const guchar *data,
                                      guint         size,
                                      GError      **error)
{

	PsdContext* context = (PsdContext*) context_ptr;
	PsdContext* ctx = context;
	
	while (size > 0) {
		switch (context->state) {
			case PSD_STATE_HEADER:
				if (feed_buffer(
						context->buffer, &context->bytes_read,
						&data, &size, PSD_HEADER_SIZE))
				{
					PsdHeader hd = psd_parse_header(ctx->buffer);

					ctx->width = hd.columns;
					ctx->height = hd.rows;
					ctx->channels = hd.channels;
					ctx->depth = hd.depth;
					ctx->color_mode = hd.color_mode;
					
					if (ctx->size_func) {
						gint w = ctx->width;
						gint h = ctx->height;
						ctx->size_func(&w, &h, ctx->user_data);
						if (w == 0 || h == 0) {
							return FALSE;
						}
					}
					
					// we need buffer that can contain one channel data of one
					// row in RLE compressed format. 2*width should be enough
					g_free(ctx->buffer);
					ctx->buffer = g_malloc(ctx->width * 2);
					
					// this will be needed for RLE decompression
					ctx->lines_lengths =
						g_malloc(2 * ctx->channels * ctx->height);
					
					ctx->pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE,
						8, ctx->width, ctx->height);

					if (ctx->lines_lengths == NULL || ctx->buffer == NULL ||
						ctx->pixbuf == NULL)
					{
						g_set_error (error, GDK_PIXBUF_ERROR,
							GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY,
							("Insufficient memory to load PSD image file"));
						return FALSE;
					}
					
					// create separate buffers for each channel
					context->channels_buffers =
						g_malloc(sizeof(guchar*) * ctx->channels);
					for (int i = 0; i <	ctx->channels; i++) {
						ctx->channels_buffers[i] =
							g_malloc(ctx->width * ctx->height);

						if (ctx->channels_buffers[i] == NULL) {
							g_set_error (error, GDK_PIXBUF_ERROR,
								GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY,
								("Insufficient memory to load PSD image file"));
							return FALSE;
						}	
					}
					
					ctx->prepared_func(ctx->pixbuf, NULL, ctx->user_data);
					
					ctx->state = PSD_STATE_COLOR_MODE_BLOCK;
					reset_context(ctx);
				}
				break;
			case PSD_STATE_COLOR_MODE_BLOCK:
				if (skip_block(ctx, &data, &size)) {
					ctx->state = PSD_STATE_RESOURCES_BLOCK;
					reset_context(ctx);
				}
				break;
			case PSD_STATE_RESOURCES_BLOCK:
				if (skip_block(ctx, &data, &size)) {
					ctx->state = PSD_STATE_LAYERS_BLOCK;
					reset_context(ctx);
				}
				break;
			case PSD_STATE_LAYERS_BLOCK:
				if (skip_block(ctx, &data, &size)) {
					ctx->state = PSD_STATE_COMPRESSION;
					reset_context(ctx);
				}
				break;
			case PSD_STATE_COMPRESSION:
				if (feed_buffer(ctx->buffer, &ctx->bytes_read, &data, &size, 2))
				{
					ctx->compression = parse_uint16(ctx->buffer);

					if (ctx->compression == PSD_COMPRESSION_RLE) {
						ctx->state = PSD_STATE_LINES_LENGTHS;
						reset_context(ctx);
					} else if (ctx->compression == PSD_COMPRESSION_NONE) {
						ctx->state = PSD_STATE_CHANNEL_DATA;
					} else {
						g_set_error (error, GDK_PIXBUF_ERROR,
							GDK_PIXBUF_ERROR_UNKNOWN_TYPE,
							("Unsupported compression type"));
						return FALSE;
					}
				}
				break;
			case PSD_STATE_LINES_LENGTHS:
				if (feed_buffer(
						(guchar*) ctx->lines_lengths, &ctx->bytes_read, &data,
						 &size,	2 * ctx->height * ctx->channels))
				{
					// convert from different endianness
					for (int i = 0; i <	ctx->height * ctx->channels; i++) {
						ctx->lines_lengths[i] = parse_uint16(
							(guchar*) &ctx->lines_lengths[i]);
					}
					ctx->state = PSD_STATE_CHANNEL_DATA;
					reset_context(ctx);
				}
				break;
			case PSD_STATE_CHANNEL_DATA:
				if (context->compression == PSD_COMPRESSION_RLE)
				{
					guint line_length = ctx->lines_lengths[
						ctx->current_channel * ctx->height + ctx->current_row];
					if (feed_buffer(ctx->buffer, &ctx->bytes_read, &data,
							&size, line_length))
					{
						ctx->bytes_read = 0;
						decompress_line(
							ctx->buffer,
							line_length,
							ctx->channels_buffers[ctx->current_channel]
								+ ctx->position
						);
						context->position += context->width;
						++context->current_row;
						
						if (ctx->current_row >= ctx->height) {
							++ctx->current_channel;
							ctx->current_row = 0;
							ctx->position = 0;
							if (ctx->current_channel >= ctx->channels) {
								ctx->state = PSD_STATE_DONE;
							}
						}
					}
				} else {
					if (feed_buffer(
							context->buffer, &context->bytes_read,
							&data, &size, context->width))
					{
						//memcpy(dest, context->buffer, context->hd.columns);
						// TODO
					}
				}
				break;
			case PSD_STATE_DONE:
			default:
				size = 0;
				break;
		}
	}
	return TRUE;
}


void
fill_vtable (GdkPixbufModule* module)
{
	//module->load = gdk_pixbuf__psd_image_load;
	module->begin_load = gdk_pixbuf__psd_image_begin_load;
	module->stop_load = gdk_pixbuf__psd_image_stop_load;
	module->load_increment = gdk_pixbuf__psd_image_load_increment;
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

