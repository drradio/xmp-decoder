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
#ifndef _XMP_MIXRATE
#define _XMP_MIXRATE 48000
#endif

typedef struct
{
    struct decoder_error error;
    xmp_context context;
    int   duration;
    char *buffer;
    int   bufferSize;
    int   consumed;
    bool  isEnd;
    int   rate;
    int   format; // Output format
} xmp_data_t;

/* See http://xmp.sourceforge.net/libxmp.html#player-parameter-setting for details */
/* Config option: mixing rate */
static int xmp_mixrate = _XMP_MIXRATE;

/* Config option: mixing format: 0 == 16 bit stereo */
static int xmp_format = 0;

/* Config option: interpolation type: nearest, linear and spline */
static int xmp_interpolation = XMP_INTERP_SPLINE;

/* Config option: stereo separation in percent: default is 70 */
static int xmp_separation = 70;

/* Config option: DSP effects: all, lowpass */
static int xmp_dsp_effects = XMP_DSP_ALL;

/* Config option: player flags: vblank, fx9bug, fixloop, a500 */
static int xmp_flags = 0;

/* Config option: default panning separation in formats with only left and right: default is 100 */
static int xmp_default_pan = 100;

/* Config option: player mode, emulate specific tracker:
 * auto, mod, noisetracker, protracker, s3m, st3, st3gus, xm, ft2, it, itsmp */
static int xmp_mode = XMP_MODE_AUTO;

/* Config option: maximum number of voices: default is 128 */
static int xmp_voices = 128;


static xmp_data_t *_xmp_load (const char *uri)
{
    xmp_data_t *mod = xmalloc (sizeof(xmp_data_t));

    mod->context  = xmp_create_context();
    mod->duration = 0;
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

static void *_xmp_open (const char *uri)
{
    xmp_data_t *mod = _xmp_load (uri);

    if (mod->error.type != ERROR_OK)
        return mod;

    /* Start player with maximal quality options */
    xmp_start_player (mod->context, xmp_mixrate,       xmp_format);
    xmp_set_player   (mod->context, XMP_PLAYER_MIX,    xmp_separation);
    xmp_set_player   (mod->context, XMP_PLAYER_INTERP, xmp_interpolation);
    xmp_set_player   (mod->context, XMP_PLAYER_DSP,    xmp_dsp_effects);
    xmp_set_player   (mod->context, XMP_PLAYER_FLAGS,  xmp_flags);
    xmp_set_player   (mod->context, XMP_PLAYER_DEFPAN, xmp_default_pan);
    xmp_set_player   (mod->context, XMP_PLAYER_MODE,   xmp_mode);
    xmp_set_player   (mod->context, XMP_PLAYER_VOICES, xmp_voices);

    struct xmp_module_info info;

    xmp_get_module_info (mod->context, &info);

    /* I don't know why do we need add 0.5 sec to duration but it works */
    mod->duration   = (info.seq_data[0].duration + 500) / 1000;
    mod->isEnd      = false;
    mod->consumed   = 0;
    mod->buffer     = NULL;
    mod->bufferSize = 0;
    mod->format     = xmp_format;
    mod->rate       = xmp_mixrate;

    return mod;
}

static void _xmp_close (void *data)
{
    xmp_data_t *mod = data;
    decoder_error_clear(&mod->error);
    xmp_end_player      (mod->context);
    xmp_release_module  (mod->context);
    xmp_free_context    (mod->context);
}

/* Based on function xmp_play_buffer() by Claudio Matsuoka and Hipolito Carraro Jr */
static int _xmp_decode (void *data, char *buffer, int size, struct sound_params *sound_params)
{
    xmp_data_t* mod = data;
    int ret = 0, filled = 0, copySize;

    sound_params->channels = mod->format & XMP_FORMAT_MONO? 1: 2;
    sound_params->fmt      = mod->format & XMP_FORMAT_8BIT? SFMT_S8: SFMT_S16 | SFMT_NE;
    sound_params->rate     = mod->rate;

    if (mod->isEnd)
        return 0;

    /* Fill buffer */
    while (filled < size)
    {
        /* Check if buffer full */
        if (mod->consumed == mod->bufferSize)
        {
            struct xmp_frame_info info;

            ret = xmp_play_frame (mod->context);
            xmp_get_frame_info (mod->context, &info);

            /* Check end of module */
            if (ret < 0 || info.loop_count >= 1)
            {
                mod->isEnd = true;

                /* Start of frame, return end of replay */
                if (!filled)
                {
                    mod->consumed   = 0;
                    mod->bufferSize = 0;
                    return 0;
                }

                /* Fill remaining of this buffer */
                memset (buffer + filled, 0, size - filled);
                return filled;
            }

            mod->consumed   = 0;
            mod->buffer     = info.buffer;
            mod->bufferSize = info.buffer_size;
        }

        /* Copy frame data to user buffer */
        copySize = MIN(size - filled, mod->bufferSize - mod->consumed);
        memcpy (buffer + filled, mod->buffer + mod->consumed, copySize);
        mod->consumed += copySize;
        filled += copySize;
    }

    return size;
}

static int _xmp_seek (void *data, int sec)
{
    xmp_data_t *mod = data;

    sec = MIN(sec, mod->duration);

    if (xmp_seek_time (mod->context, sec * 1000 - 500) < 0)
        return -1;

    return sec;
}

static void _xmp_info (const char *uri, struct file_tags *tags, const int selected)
{
    xmp_data_t *mod = _xmp_load (uri);

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

    _xmp_close (mod);
}

/* Modules don't have bitrate */
static int _xmp_get_bitrate (void *data ATTR_UNUSED)
{
    return -1;
}

static int _xmp_get_duration (void *data)
{
    return ((xmp_data_t*)data)->duration;
}

static void _xmp_get_error (void *data, struct decoder_error *error)
{
    decoder_error_copy (error, &((xmp_data_t*)data)->error);
}

/* TODO: add more supported extensions */
static int _xmp_our_format_ext (const char *ext)
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
    _xmp_open,
    NULL,
    NULL,
    _xmp_close,
    _xmp_decode,
    _xmp_seek,
    _xmp_info,
    _xmp_get_bitrate,
    _xmp_get_duration,
    _xmp_get_error,
    _xmp_our_format_ext,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

static struct playermode
{
    char *string;
    int   value;
} playermodes[] =
{
    {"AUTO",         XMP_MODE_AUTO},
    {"MOD",          XMP_MODE_MOD},
    {"NOISETRACKER", XMP_MODE_NOISETRACKER},
    {"PROTRACKER",   XMP_MODE_PROTRACKER},
    {"S3M",          XMP_MODE_S3M},
    {"ST3",          XMP_MODE_ST3},
    {"ST3GUS",       XMP_MODE_ST3GUS},
    {"XM",           XMP_MODE_XM},
    {"FT2",          XMP_MODE_FT2},
    {"IT",           XMP_MODE_IT},
    {"ITSMP",        XMP_MODE_ITSMP},
};

struct decoder *plugin_init()
{
    char *value = NULL;
    size_t i;

    /* Get numeric options */
    xmp_mixrate     = options_get_int ("XMP_MixingRate");
    xmp_voices      = options_get_int ("XMP_Voices");
    xmp_separation  = options_get_int ("XMP_StereoSeparation");
    xmp_default_pan = options_get_int ("XMP_DefaultPan");

    if (options_get_bool ("XMP_8bit"))
        xmp_format |= XMP_FORMAT_8BIT;

    if (options_get_bool ("XMP_Mono"))
        xmp_format |= XMP_FORMAT_MONO;

    /* Get interpolation mode */
    value = options_get_symb ("XMP_Interpolation");
    if (!strcasecmp(value, "NEAREST"))
        xmp_interpolation = XMP_INTERP_NEAREST;
    else if (!strcasecmp(value, "LINEAR"))
        xmp_interpolation = XMP_INTERP_LINEAR;
    else if (!strcasecmp(value, "SPLINE"))
        xmp_interpolation = XMP_INTERP_SPLINE;

    /* Get DSP effects mode */
    value = options_get_symb ("XMP_DSPEffects");
    if (!strcasecmp(value, "ALL"))
        xmp_dsp_effects = XMP_DSP_ALL;
    else if (!strcasecmp(value, "LOWPASS"))
        xmp_dsp_effects = XMP_DSP_LOWPASS;

    /* Get player mode */
    value = options_get_symb ("XMP_PlayerMode");
    for (i = 0; i < sizeof(playermodes)/sizeof(struct playermode); i++)
    {
        if (!strcasecmp(value, playermodes[i].string))
        {
            xmp_mode = playermodes[i].value;
            break;
        }
    }

    /* Get player flags */
    lists_t_strs *list = options_get_list ("XMP_PlayerFlags");
    if (lists_strs_exists(list, "VBLANK"))
        xmp_flags |= XMP_FLAGS_VBLANK;
    if (lists_strs_exists(list, "FX9BUG"))
        xmp_flags |= XMP_FLAGS_FX9BUG;
    if (lists_strs_exists(list, "FIXLOOP"))
        xmp_flags |= XMP_FLAGS_FIXLOOP;
    if (lists_strs_exists(list, "A500"))
        xmp_flags |= XMP_FLAGS_A500;

    return &xmp_decoder;
}
