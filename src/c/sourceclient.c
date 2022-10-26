/*
#   sourceclient.c: the streaming module of idjc
#   Copyright (C) 2007 Stephen Fairchild (s-fairchild@users.sourceforge.net)
#
#   This program is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 2 of the License, or
#   (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program in the file entitled COPYING.
#   If not, see <http://www.gnu.org/licenses/>.
*/

#include "../config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <unistd.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include "sourceclient.h"
#include "kvpparse.h"
#include "live_ogg_encoder.h"
#include "live_aac_encoder.h"
#include "sig.h"
#include "main.h"

static int threads_up;

static void threads_init(struct threads_info *ti)
    {
    int i;
    
    ti->n_encoders = atoi(getenv("num_encoders"));
    ti->n_streamers = atoi(getenv("num_streamers"));
    ti->n_recorders = atoi(getenv("num_recorders"));
    ti->encoder = calloc(ti->n_encoders, sizeof (struct encoder *));
    ti->streamer = calloc(ti->n_streamers, sizeof (struct streamer *));
    ti->recorder = calloc(ti->n_recorders, sizeof (struct recorder *));
    if (!(ti->encoder && ti->streamer && ti->recorder))
        {
        fprintf(stderr, "threads_init: malloc failure\n");
        exit(5);
        }
    for (i = 0; i < ti->n_encoders; i++)
        if (!(ti->encoder[i] = encoder_init(ti, i)))
            {
            fprintf(stderr, "threads_init: encoder initialisation failed\n");
            exit(5);
            }
    for (i = 0; i < ti->n_streamers; i++)
        if (!(ti->streamer[i] = streamer_init(ti, i)))
            {
            fprintf(stderr, "threads_init: streamer initialisation failed\n");
            exit(5);
            }
    for (i = 0; i < ti->n_recorders; i++)
        if (!(ti->recorder[i] = recorder_init(ti, i)))
            {
            fprintf(stderr, "threads_init: recorder initialisation failed\n");
            exit(5);
            }
    if (!(ti->audio_feed = audio_feed_init(ti)))
        {
        fprintf(stderr, "threads_init: audio feed initialisation failed\n");
        exit(5);
        }
    fprintf(stderr, "started %d encoders, %d streamers, %d recorders\n", ti->n_encoders, ti->n_streamers, ti->n_recorders);
    threads_up = TRUE;
    }

static void threads_shutdown(struct threads_info *ti)
    {
    int i;
    
    if (threads_up)
        {
        for (i = 0; i < ti->n_recorders; i++)
            recorder_destroy(ti->recorder[i]);
        for (i = 0; i < ti->n_streamers; i++)
            streamer_destroy(ti->streamer[i]);
        for (i = 0; i < ti->n_encoders; i++)
            encoder_destroy(ti->encoder[i]);
        free(ti->recorder);
        free(ti->streamer);
        free(ti->encoder);
        audio_feed_destroy(ti->audio_feed);
        }
    }

static int get_report(struct threads_info *ti, struct universal_vars *uv, void *other)
    {
    if (!strcmp(uv->dev_type, "streamer"))
        {
        if (uv->tab >= 0 && uv->tab < ti->n_streamers)
            return streamer_make_report(ti->streamer[uv->tab]);
        fprintf(stderr, "get_report: streamer %s does not exist\n", uv->tab_id);
        return FAILED;
        }
    if (!strcmp(uv->dev_type, "recorder"))
        {
        if (uv->tab >= 0 && uv->tab < ti->n_recorders)
            return recorder_make_report(ti->recorder[uv->tab]);
        fprintf(stderr, "get_report: recorder %s does not exist\n", uv->tab_id); 
        return FAILED;
        }
    if (!strcmp(uv->dev_type, "encoder"))
        return FAILED;
    fprintf(stderr, "get_report: unhandled dev_type %s\n", uv->dev_type);
    return FAILED;
    }

static int command_parse(struct commandmap *map, struct threads_info *ti, struct universal_vars *uv)
    {
    for (; map->key; map++)
        if (!(strcmp(uv->command, map->key)))
            {
            if (uv->tab_id)
                uv->tab = atoi(uv->tab_id);
            return map->function(ti, uv, map->other_parameter);
            }
    fprintf(stderr, "command_parse: unhandled command %s\n", uv->command);
    return FAILED;
    }

void comms_send(char *message)
    {
    fprintf(g.out, "idjcsc: %s\n", message);
    fflush(g.out);
    }

static struct threads_info ti;
static struct encoder_vars ev;
static struct streamer_vars sv;
static struct recorder_vars rv;
static struct universal_vars uv;

static struct kvpdict kvpdict[] = {
    { "encode_source",    &ev.encode_source, NULL },        /* encoder_vars */
    { "samplerate",       &ev.samplerate, NULL },
    { "resample_quality", &ev.resample_quality, NULL },
    { "family",           &ev.family, NULL },
    { "codec",            &ev.codec, NULL },
    { "bitrate",          &ev.bitrate, NULL },
    { "variability",      &ev.variability, NULL },
    { "bitwidth",         &ev.bitwidth, NULL },
    { "mode",             &ev.mode, NULL },
    { "metadata_mode",    &ev.metadata_mode, NULL },
    { "standard",         &ev.standard, NULL },
    { "pregain",          &ev.pregain, NULL },
    { "postgain",         &ev.postgain, NULL },
    { "quality",          &ev.quality, NULL },
    { "complexity",       &ev.complexity, NULL },
    { "framesize",        &ev.framesize, NULL },
    { "filename",         &ev.filename, NULL },
    { "offset",           &ev.offset, NULL },
    { "custom_meta",      &ev.custom_meta, NULL },
    { "artist",           &ev.artist, NULL },
    { "title",            &ev.title, NULL },
    { "album",            &ev.album, NULL },
    { "stream_source",    &sv.stream_source, NULL },        /* streamer_vars */
    { "server_type",      &sv.server_type, NULL },
    { "host",             &sv.host, NULL },
    { "port",             &sv.port, NULL },
    { "mount",            &sv.mount, NULL },
    { "login",            &sv.login, NULL },
    { "password",         &sv.password, NULL },
    { "useragent",        &sv.useragent, NULL },
    { "dj_name",          &sv.dj_name, NULL },
    { "listen_url",       &sv.listen_url, NULL },
    { "description",      &sv.description, NULL },
    { "genre",            &sv.genre, NULL },
    { "irc",              &sv.irc, NULL },
    { "aim",              &sv.aim, NULL },
    { "icq",              &sv.icq, NULL },
    { "tls",              &sv.tls, NULL },
    { "ca_directory",     &sv.ca_dir, NULL },
    { "ca_file",          &sv.ca_file, NULL },
    { "client_cert",      &sv.client_cert, NULL },
    { "make_public",      &sv.make_public, NULL },
    { "record_source",    &rv.record_source, NULL },        /* recorder_vars */
    { "record_filename",  &rv.record_filename, NULL },
    { "record_folder",    &rv.record_folder, NULL },
    { "pause_button",     &rv.pause_button, NULL },
    { "command",  &uv.command, NULL},
    { "dev_type", &uv.dev_type, NULL},
    { "tab_id",   &uv.tab_id, NULL},
    { NULL, NULL, NULL } };

static struct commandmap commandmap[] = {
    { "jack_samplerate_request", audio_feed_jack_samplerate_request, NULL },
    { "encoder_lame_availability", encoder_init_lame, NULL},
    { "get_report", get_report, NULL },
    { "encoder_start", encoder_start, &ev },
    { "encoder_stop", encoder_stop, NULL },
    { "encoder_update", encoder_update, &ev },
    { "new_song_metadata", encoder_new_song_metadata, &ev },
    { "new_custom_metadata", encoder_new_custom_metadata, &ev },
    { "recorder_start", recorder_start, &rv },
    { "recorder_stop", recorder_stop, NULL },
    { "recorder_pause", recorder_pause, &rv },
    { "recorder_unpause", recorder_unpause, &rv },
    { "server_connect", streamer_connect, &sv },
    { "server_disconnect", streamer_disconnect, NULL },
    { "initiate_fade", encoder_initiate_fade, NULL },
    { NULL, NULL, NULL } }; 

static void sourceclient_cleanup()
    {
    threads_shutdown(&ti);
    kvp_free_dict(kvpdict);
    }

void sourceclient_init()
    {
    sig_init();
    setenv("LC_ALL", "C", 1);
    setlocale(LC_ALL, "C");

    srand(time(NULL));
    
    threads_init(&ti);
    atexit(sourceclient_cleanup);
    }

int sourceclient_main()
    {
    if (!kvp_parse(kvpdict, g.in))
        return FALSE;

    if (uv.command && command_parse(commandmap, &ti, &uv))
        comms_send("succeeded");
    else
        {
        fprintf(stderr, "command failed for command: %s\n", uv.command);
        comms_send("failed");
        }
    if (uv.command)
        {
        free(uv.command);
        uv.command = NULL;
        }
        
    return TRUE;
    }
