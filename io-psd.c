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
 * - MODULE_ENTRY stuff
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
	guchar  resetved[6];
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

	guint32            width;         /* width of image in pixels (1-30000) */
	guint32            height;        /* height of image in pixels (1-30000) */
	guint16            channels;      /* number of color channels (1-24) */
	guint16            depth;         /* number of bits per channel (1, 8, and 16) */
	PsdColorMode       color_mode;
	PsdCompressionType compression;

	guchar**           ch_bufs;       /* channels buffers */
	guint              curr_ch;       /* current channel */
	guint              curr_row;
	guint              pos;           // redundant?
	guint16*           lines_lengths;
	gboolean           finalized;
} PsdContext;


static guint16
read_uint16 (guchar* buf)
{
	return (buf[0] << 8) | buf[1];
}

static guint32
read_uint32 (guchar* buf)
{
	return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
}


/*
 * Parse Psdheader from buffer
 *
 * str is expected to be at least PSD_HEADER_SIZE long
 */
static PsdHeader
psd_parse_header (guchar* str)
{
	PsdHeader hd;
	
	memcpy(hd.signature, str, 4);
	hd.version = read_uint16(str + 4);
	hd.channels = read_uint16(str + 12);
	hd.rows = read_uint32(str + 14);
	hd.columns = read_uint32(str + 18);
	hd.depth = read_uint16(str + 22);
	hd.color_mode = read_uint16(str + 24);

	return hd;
}

/*
 * Attempts to read bytes_needed bytes from data and stores them in buffer.
 *
 * Returns true if there were enough bytes and false otherwise
 * (which means we need to call feed_buffer again)
 */
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

/*
 * Attempts to read size of the block and then skip this block.
 *
 * Returns true when finishes consuming block data, otherwise false
 * (false means we must call skip_block once again)
 */
static gboolean
skip_block (PsdContext* context, const guchar** data, guint* size)
{
	static guint counter;

	if (!context->bytes_to_skip_known) {
		context->bytes_read = 0;
		if (feed_buffer(context->buffer, &context->bytes_read, data, size, 4)) {
			context->bytes_to_skip = read_uint32(context->buffer);
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

/*
 * Decodes RLE-compressed data
 */
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
reset_context_buffer(PsdContext* ctx)
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
	context->buffer = g_malloc(PSD_HEADER_SIZE);
	reset_context_buffer(context);

	context->ch_bufs = NULL;
	context->curr_ch = 0;
	context->curr_row = 0;
	context->pos = 0;
	context->lines_lengths = NULL;
	context->finalized = FALSE;

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
	}
	
	g_free(ctx->buffer);
	g_free(ctx->lines_lengths);
	for (int i = 0; i < ctx->channels; i++) {
		g_free(ctx->ch_bufs[i]);
	}
	g_free(ctx);
	
	return retval;
}


static gboolean
gdk_pixbuf__psd_image_load_increment (gpointer      context_ptr,
                                      const guchar *data,
                                      guint         size,
                                      GError      **error)
{

	PsdContext* ctx = (PsdContext*) context_ptr;
	
	while (size > 0) {
		switch (ctx->state) {
			case PSD_STATE_HEADER:
				if (feed_buffer(
						ctx->buffer, &ctx->bytes_read,
						&data, &size, PSD_HEADER_SIZE))
				{
					PsdHeader hd = psd_parse_header(ctx->buffer);

					ctx->width = hd.columns;
					ctx->height = hd.rows;
					ctx->channels = hd.channels;
					ctx->depth = hd.depth;
					ctx->color_mode = hd.color_mode;
					
					//g_message("color_mode=%d, channels=%d, depth=%d",
					//	ctx->color_mode, ctx->channels, ctx->depth);
					
					if (ctx->color_mode != PSD_MODE_RGB/* &&
					    ctx->color_mode != PSD_MODE_CMYK*/
					) {
						g_set_error (error, GDK_PIXBUF_ERROR,
							GDK_PIXBUF_ERROR_UNKNOWN_TYPE,
							("Unsupported color mode"));
						return FALSE;
					}
					
					if (ctx->depth != 8) {
						g_set_error (error, GDK_PIXBUF_ERROR,
							GDK_PIXBUF_ERROR_UNKNOWN_TYPE,
							("Unsupported color depth"));
						return FALSE;
					}
					
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
					ctx->ch_bufs = g_malloc(sizeof(guchar*) * ctx->channels);
					for (int i = 0; i <	ctx->channels; i++) {
						ctx->ch_bufs[i] =
							g_malloc(ctx->width * ctx->height);

						if (ctx->ch_bufs[i] == NULL) {
							g_set_error (error, GDK_PIXBUF_ERROR,
								GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY,
								("Insufficient memory to load PSD image file"));
							return FALSE;
						}	
					}
					
					ctx->prepared_func(ctx->pixbuf, NULL, ctx->user_data);
					
					ctx->state = PSD_STATE_COLOR_MODE_BLOCK;
					reset_context_buffer(ctx);
				}
				break;
			case PSD_STATE_COLOR_MODE_BLOCK:
				if (skip_block(ctx, &data, &size)) {
					ctx->state = PSD_STATE_RESOURCES_BLOCK;
					reset_context_buffer(ctx);
				}
				break;
			case PSD_STATE_RESOURCES_BLOCK:
				if (skip_block(ctx, &data, &size)) {
					ctx->state = PSD_STATE_LAYERS_BLOCK;
					reset_context_buffer(ctx);
				}
				break;
			case PSD_STATE_LAYERS_BLOCK:
				if (skip_block(ctx, &data, &size)) {
					ctx->state = PSD_STATE_COMPRESSION;
					reset_context_buffer(ctx);
				}
				break;
			case PSD_STATE_COMPRESSION:
				if (feed_buffer(ctx->buffer, &ctx->bytes_read, &data, &size, 2))
				{
					ctx->compression = read_uint16(ctx->buffer);

					if (ctx->compression == PSD_COMPRESSION_RLE) {
						ctx->state = PSD_STATE_LINES_LENGTHS;
						reset_context_buffer(ctx);
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
						ctx->lines_lengths[i] = read_uint16(
							(guchar*) &ctx->lines_lengths[i]);
					}
					ctx->state = PSD_STATE_CHANNEL_DATA;
					reset_context_buffer(ctx);
				}
				break;
			case PSD_STATE_CHANNEL_DATA:
				{
					guint line_length = ctx->width;
					if (ctx->compression == PSD_COMPRESSION_RLE) {
						line_length = ctx->lines_lengths[
							ctx->curr_ch * ctx->height + ctx->curr_row];
					}
					
					if (feed_buffer(ctx->buffer, &ctx->bytes_read, &data, &size,
							line_length))
					{
						reset_context_buffer(ctx);
					
						if (ctx->compression == PSD_COMPRESSION_RLE) {
							decompress_line(ctx->buffer, line_length,
								ctx->ch_bufs[ctx->curr_ch] + ctx->pos
							);
						} else {
							memcpy(ctx->ch_bufs[ctx->curr_ch] + ctx->pos,
								ctx->buffer, ctx->width);
						}

						ctx->pos += ctx->width;
						++ctx->curr_row;
					
						if (ctx->curr_row >= ctx->height) {
							++ctx->curr_ch;
							ctx->curr_row = 0;
							ctx->pos = 0;
							if (ctx->curr_ch >= ctx->channels) {
								ctx->state = PSD_STATE_DONE;
							}
						}
					}
				}
				break;
			case PSD_STATE_DONE:
			default:
				size = 0;
				break;
		}
	}
	
	if (ctx->state == PSD_STATE_DONE && !ctx->finalized) {
		// convert or copy channel buffers to our GdkPixbuf
		if (ctx->color_mode == PSD_MODE_RGB) {
			guchar* pixels = gdk_pixbuf_get_pixels(ctx->pixbuf);
			for (int i = 0; i < ctx->height; i++) {
				for (int j = 0; j < ctx->width; j++) {
					pixels[3*j+0] = ctx->ch_bufs[0][ctx->width*i + j];
					pixels[3*j+1] = ctx->ch_bufs[1][ctx->width*i + j];
					pixels[3*j+2] = ctx->ch_bufs[2][ctx->width*i + j];
				}
				pixels += gdk_pixbuf_get_rowstride(ctx->pixbuf);
			}
		}
		ctx->finalized = TRUE;
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

