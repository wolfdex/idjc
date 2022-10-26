/*
#   main.c: backend unificaction module.
#   Copyright (C) 2012 Stephen Fairchild (s-fairchild@users.sourceforge.net)
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
#include <locale.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <jack/session.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef HAVE_LIBAV
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#endif /* HAVE_LIBAV */

#include "sig.h"
#include "mixer.h"
#include "sourceclient.h"
#include "main.h"

#define FALSE 0
#define TRUE (!FALSE)

struct globs g;

static void alarm_handler(int sig)
    {
    if (g.app_shutdown)
        exit(5);

    if (g.mixer_up && !mixer_healthcheck())
        g.app_shutdown = TRUE;

    if (g.jack_timeout++ > 9)
        g.app_shutdown = TRUE;
    
    if (g.has_head && g.main_timeout++ > 9)
        g.app_shutdown = TRUE;

    /* One second grace to shut down naturally. */
    alarm(1);
    }

static void custom_jack_error_callback(const char *message)
    {
    fprintf(stderr, "jack error: %s\n", message);
    }

static void custom_jack_info_callback(const char *message)
    {
    fprintf(stderr, "jack info: %s\n", message);
    }

static void custom_jack_on_shutdown_callback()
    {
    g.app_shutdown = TRUE;
    }

static void session_callback(jack_session_event_t *event, void *arg)
    {
    /* Store the address of the event so the data can be retrieved later by
     * user interface polling. This is done in mixer.c.
     */
    if (jack_ringbuffer_write(g.session_event_rb,
                                (char *)&event, sizeof event) < sizeof event)
        {
        /* The ringbuffer is good for 512 writes in 1/20th second. (32 bit) */
        fprintf(stderr,
                    "main.c: session event ringbuffer is stuffed -- exiting\n");
        exit(5);
        }
    }
    
static int buffer_size_callback(jack_nframes_t n_frames, void *arg)
    {
    return mixer_new_buffer_size(n_frames);
    }
    
static void freewheel_callback(int starting, void *arg)
    {
    g.freewheel = starting;
    }

static void cleanup_jack()
    {
    if (g.client)
        {
        jack_deactivate(g.client);
        jack_client_close(g.client);
        }
    }

static int main_process_audio(jack_nframes_t n_frames, void *arg)
    {
    int rv;

    rv =  mixer_process_audio(n_frames, arg) || audio_feed_process_audio(n_frames, arg);
    
    if (rv == 0)
        g.jack_timeout = 0;
    
    return rv;
    }

static int backend_main()
    {
    char *buffer = NULL;
    size_t n = 10;
    int keep_running = TRUE;
    jack_options_t options = 0;

    /* Without these being set the backend will segfault. */
        {
        int o = FALSE;    /* Overwrite flag */
        if (setenv("session_type", "L0", o) ||
                setenv("client_id", "idjc_nofrontend", o) ||
                setenv("mic_qty", "4", o) ||
                setenv("num_streamers", "6", o) ||
                setenv("num_encoders", "6", o) ||
                setenv("num_recorders", "2", o) ||
                setenv("num_effects", "24", o) ||
                setenv("jack_parameter", "default", o) ||
                setenv("has_head", "0", o) ||
                /* C locale required for . as radix character. */
                setenv("LC_ALL", "C", 1))
            {
            perror("main: failed to set environment variable");
            exit(5);
            }
        }

    setlocale(LC_ALL, getenv("LC_ALL"));
    g.has_head = atoi(getenv("has_head"));
    signal(SIGALRM, alarm_handler);
    
    /* Signal handling. */
    sig_init();

    if (!(strcmp(getenv("session_type"), "JACK")))
        {
        options = JackSessionID;
        g.session_event_rb = jack_ringbuffer_create(2048);
        }

    else
        options = JackUseExactName | JackServerName;

    if ((g.client = jack_client_open(getenv("client_id"), options, NULL, getenv("jack_parameter"))) == 0)
        {
        fprintf(stderr, "main.c: jack_client_open failed");
        exit(5);
        }

#ifdef HAVE_LIBAV
    if (pthread_mutex_init(&g.avc_mutex, NULL))
        {
        fprintf(stderr, "pthread_mutex_init failed\n");
        exit(5);
        }
#ifdef HAVE_AVCODEC_REGISTER_ALL
    // kept for old ffmpeg compatibilty
    avcodec_register_all();
#endif
#ifdef HAVE_AV_REGISTER_ALL
    // kept for old ffmpeg compatibility
    av_register_all();
#endif
#endif /* HAVE_LIBAV */

    alarm(3);

    jack_set_error_function(custom_jack_error_callback);
    jack_set_info_function(custom_jack_info_callback);
    jack_on_shutdown(g.client, custom_jack_on_shutdown_callback, NULL);

    jack_set_freewheel_callback(g.client, freewheel_callback, NULL);
    jack_set_session_callback(g.client, session_callback, NULL);
    jack_set_process_callback(g.client, main_process_audio, NULL);
    jack_set_buffer_size_callback(g.client, buffer_size_callback, NULL);

    /* Registration of JACK ports. */
    #define MK_AUDIO_INPUT(var, name) var = jack_port_register(g.client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    #define MK_AUDIO_OUTPUT(var, name) var = jack_port_register(g.client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    
        {
        struct jack_ports *p = &g.port;

        /* Mixer ports. */
        MK_AUDIO_OUTPUT(p->dj_out_l, "dj_out_l");
        MK_AUDIO_OUTPUT(p->dj_out_r, "dj_out_r");
        MK_AUDIO_OUTPUT(p->dsp_out_l, "dsp_out_l");
        MK_AUDIO_OUTPUT(p->dsp_out_r, "dsp_out_r");
        MK_AUDIO_INPUT(p->dsp_in_l, "dsp_in_l");
        MK_AUDIO_INPUT(p->dsp_in_r, "dsp_in_r");
        MK_AUDIO_OUTPUT(p->str_out_l, "str_out_l");
        MK_AUDIO_OUTPUT(p->str_out_r, "str_out_r");
        MK_AUDIO_OUTPUT(p->voip_out_l, "voip_out_l");
        MK_AUDIO_OUTPUT(p->voip_out_r, "voip_out_r");
        MK_AUDIO_INPUT(p->voip_in_l, "voip_in_l");
        MK_AUDIO_INPUT(p->voip_in_r, "voip_in_r");
        MK_AUDIO_OUTPUT(p->alarm_out, "alarm_out");
        /* Player related ports. */
        MK_AUDIO_OUTPUT(p->pl_out_l, "pl_out_l");
        MK_AUDIO_OUTPUT(p->pl_out_r, "pl_out_r");
        MK_AUDIO_OUTPUT(p->pr_out_l, "pr_out_l");
        MK_AUDIO_OUTPUT(p->pr_out_r, "pr_out_r");
        MK_AUDIO_OUTPUT(p->pi_out_l, "pi_out_l");
        MK_AUDIO_OUTPUT(p->pi_out_r, "pi_out_r");
        MK_AUDIO_OUTPUT(p->pe1_out_l, "pe01-12_out_l");
        MK_AUDIO_OUTPUT(p->pe1_out_r, "pe01-12_out_r");
        MK_AUDIO_OUTPUT(p->pe2_out_l, "pe13-24_out_l");
        MK_AUDIO_OUTPUT(p->pe2_out_r, "pe13-24_out_r");
        MK_AUDIO_INPUT(p->pl_in_l, "pl_in_l");
        MK_AUDIO_INPUT(p->pl_in_r, "pl_in_r");
        MK_AUDIO_INPUT(p->pr_in_l, "pr_in_l");
        MK_AUDIO_INPUT(p->pr_in_r, "pr_in_r");
        MK_AUDIO_INPUT(p->pi_in_l, "pi_in_l");
        MK_AUDIO_INPUT(p->pi_in_r, "pi_in_r");
        MK_AUDIO_INPUT(p->pe_in_l, "pe_in_l");
        MK_AUDIO_INPUT(p->pe_in_r, "pe_in_r");

        /* Not really a mixer port but handled in the mixer code. */
        p->midi_port = jack_port_register(g.client, "midi_control", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);

        /* Sourceclient ports. */
        MK_AUDIO_INPUT(p->output_in_l, "output_in_l");
        MK_AUDIO_INPUT(p->output_in_r, "output_in_r");
        }

    #undef MK_AUDIO_INPUT
    #undef MK_AUDIO_OUTPUT

    /* Submodule initialization. */
    mixer_init();
    sourceclient_init();

    if (jack_activate(g.client))
        {
        fprintf(stderr, "main.c: failed to activate JACK client.\n");
        jack_client_close(g.client);
        g.client = NULL;
        exit(5);
        }
    atexit(cleanup_jack);

    fprintf(g.out, "idjc backend ready\n");
    fflush(g.out);

    alarm(1);

    while (keep_running && getline(&buffer, &n, g.in) > 0 && !g.app_shutdown)
        {
        /* Filter commands to submodules. */
        if (!strcmp(buffer, "mx\n"))
            keep_running = mixer_main();
        else
            {
            if (!strcmp(buffer, "sc\n"))
                keep_running = sourceclient_main();
            else
                {
                fprintf(stderr, "main.c: expected module name, got: %s", buffer);
                exit(5);
                }
            }
            
        g.main_timeout = 0;
        }

    jack_deactivate(g.client);
    jack_client_close(g.client);
    g.client = NULL;

    alarm(0);
    
    if (buffer)
        free(buffer);

    if (g.session_event_rb)
        jack_ringbuffer_free(g.session_event_rb);

    return 0;
    }

int init_backend(int *read_pipe, int *write_pipe)
    {
    char *ui2be = getenv("ui2be");
    char *be2ui = getenv("be2ui");
    pid_t pid;

    unlink(ui2be);
    unlink(be2ui);
    if (mkfifo(ui2be, S_IWUSR | S_IRUSR) || mkfifo(be2ui, S_IWUSR | S_IRUSR))
        {
        fprintf(stderr, "init_backend: failed to make fifo\n");
        return -1;
        }

    if (!(pid = fork()))
        {
        int maxfd = sysconf(_SC_OPEN_MAX);

        for (int fd = 3; fd < maxfd; ++fd)
            close(fd);

        if ((g.in = fopen(ui2be, "r")) && (g.out = fopen(be2ui, "w")))
            {
            fputc('#', g.out);
                
            int ret = backend_main();
            fclose(g.in);
            fclose(g.out);
            exit(ret);
            }
        else
            fprintf(stderr, "init_backend: in fork: failed to open fifo\n");
        }

    *write_pipe = open(ui2be, O_WRONLY);
    *read_pipe = open(be2ui, O_RDONLY);
    
    char buffer;
    if (read(*read_pipe, &buffer, 1) != 1)
        {
        fprintf(stderr, "init_backend: pipe failed\n");
        return -1;
        }
    
    return (int)pid;
    }
