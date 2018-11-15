/*
 * Copyright(C) 2018 - Nagashybay <Dr.Radio> Zhanibek <njm.janik@yandex.ru>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* Alternative module player plugin based on libxmp.
 * I have wrote it because libmodplug plays some modules incorrectly,
 * especialy that ones with stereo samples and filter effects.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <strings.h>
#include <assert.h>
#include <errno.h>
#include <xmp.h>

#define DEBUG

#include "common.h"
#include "io.h"
#include "decoder.h"
#include "log.h"
#include "files.h"
#include "options.h"

/* Hardcoded options */
#ifndef OUTPUTRATE
#define OUTPUTRATE 44100
#endif

typedef struct
{
    xmp_context context;
    int length;
    struct decoder_error error;
} xmp_data_t;


static xmp_data_t *loadmod (const char *uri)
{
    xmp_data_t *mod = xmalloc (sizeof(xmp_data_t));

    mod->context = xmp_create_context();
    mod->length  = 0;
    decoder_error_init (&mod->error);

    struct io_stream *s = io_open (uri, 0);
    if(!io_ok(s))
    {
        decoder_error (&mod->error, ERROR_FATAL, 0, "Can't open file: %s", uri);
        io_close (s);
        return mod;
    }

    off_t size = io_file_size(s);

    /* Are there modules so big as 2 Gbytes? */
    if (!RANGE(1, size, INT_MAX))
    {
        decoder_error (&mod->error, ERROR_FATAL, 0, "Module size unsuitable for loading: %s", uri);
        io_close (s);
        return mod;
    }

    char *filedata = xmalloc (size);

    io_read (s, filedata, size);
    io_close (s);

    int ret = xmp_load_module_from_memory (mod->context, filedata, size);

    switch (ret)
    {
        case -XMP_ERROR_FORMAT:
            decoder_error (&mod->error, ERROR_FATAL, 0, "Unrecognized module format: %s", uri);
            break;

        case -XMP_ERROR_LOAD:
            decoder_error (&mod->error, ERROR_FATAL, 0, "Module loading failed: %s", uri);
            break;

        case -XMP_ERROR_SYSTEM:
            decoder_error (&mod->error, ERROR_FATAL, 0, "System error: %s: %s", uri, strerror(errno));
            break;

        default:
            break;
    }

    free (filedata);

    return mod;
}

static void *__open (const char *uri)
{
    xmp_data_t *mod = loadmod (uri);

    if (mod->error.type != ERROR_OK)
        return mod;

    /* Start player with maximal quality options */
    xmp_start_player (mod->context, OUTPUTRATE, 0);

    struct xmp_module_info info;

    xmp_get_module_info (mod->context, &info);

    /* I don't know why do we need add 0.5 sec to length but it works */
    mod->length = (info.seq_data[0].duration + 500) / 1000;

    return mod;
}

static void __close (void *data)
{
    xmp_data_t *mod = data;
    decoder_error_clear(&mod->error);
    xmp_end_player      (mod->context);
    xmp_release_module  (mod->context);
    xmp_free_context    (mod->context);
}

static int __decode (void *data, char *buf, int len, struct sound_params *sound_params)
{
    sound_params->channels = 2;
    sound_params->rate = OUTPUTRATE;
    sound_params->fmt = SFMT_S16 | SFMT_NE;
    int ret = xmp_play_buffer (((xmp_data_t*)data)->context, buf, len, 1);
    return ret == -XMP_END? 0: len;
}

static int __seek (void *data, int sec)
{
    xmp_data_t *mod = data;

    sec = MIN(sec, mod->length);

    if (xmp_seek_time (mod->context, sec * 1000 - 500) == -XMP_ERROR_STATE)
        return -1;

    return sec;
}

static void __info (const char *uri, struct file_tags *tags, const int selected)
{
    xmp_data_t *mod = loadmod (uri);

    if (mod->error.type != ERROR_OK)
        return;

    struct xmp_module_info info;

    xmp_get_module_info (mod->context, &info);

    if(selected & TAGS_TIME)
    {
        tags->time = (info.seq_data[0].duration + 500) / 1000;
        tags->filled |= TAGS_TIME;
    }

    if(selected & TAGS_COMMENTS)
    {
        tags->title = xstrdup(info.mod->name);
        tags->filled |= TAGS_COMMENTS;
    }

    __close (mod);
}

/* Modules don't have bitrate */
static int __get_bitrate (void *data ATTR_UNUSED)
{
    return -1;
}

static int __get_duration (void *data)
{
    return ((xmp_data_t*)data)->length;
}

static void __get_error (void *data, struct decoder_error *error)
{
    decoder_error_copy (error, &((xmp_data_t*)data)->error);
}

/* TODO: add more supported extensions */
static int __our_format_ext (const char *ext)
{
    return !strcasecmp (ext, "NONE") ||
           !strcasecmp (ext, "XM")   ||
           !strcasecmp (ext, "MOD")  ||
           !strcasecmp (ext, "FLT")  ||
           !strcasecmp (ext, "ST")   ||
           !strcasecmp (ext, "IT")   ||
           !strcasecmp (ext, "S3M")  ||
           !strcasecmp (ext, "STM")  ||
           !strcasecmp (ext, "STX")  ||
           !strcasecmp (ext, "MTM")  ||
           !strcasecmp (ext, "ICE")  ||
           !strcasecmp (ext, "IMF")  ||
           !strcasecmp (ext, "PTM")  ||
           !strcasecmp (ext, "MDL")  ||
           !strcasecmp (ext, "ULT")  ||
           !strcasecmp (ext, "LIQ")  ||
           !strcasecmp (ext, "PSM")  ||
           !strcasecmp (ext, "MED")  ||
           !strcasecmp (ext, "669")  ||
           !strcasecmp (ext, "FAR")  ||
           !strcasecmp (ext, "AMF")  ||
           !strcasecmp (ext, "AMS")  ||
           !strcasecmp (ext, "DSM")  ||
           !strcasecmp (ext, "OKT")  ||
           !strcasecmp (ext, "DBM")  ||
           !strcasecmp (ext, "MT2")  ||
           !strcasecmp (ext, "AMF0") ||
           !strcasecmp (ext, "J2B")  ||
           !strcasecmp (ext, "UMX");
}


static struct decoder xmp_decoder =
{
    DECODER_API_VERSION,
    NULL,
    NULL,
    __open,
    NULL,
    NULL,
    __close,
    __decode,
    __seek,
    __info,
    __get_bitrate,
    __get_duration,
    __get_error,
    __our_format_ext,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

struct decoder *plugin_init()
{
    return &xmp_decoder;
}
