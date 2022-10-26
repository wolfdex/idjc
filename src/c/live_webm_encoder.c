/*
#   live_webm_encoder.c: encode using libavformat
#   Copyright (C) 2015 Stephen Fairchild (s-fairchild@users.sourceforge.net)
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

#ifdef HAVE_AVCODEC

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include "main.h"
#include "sourceclient.h"
#include "live_webm_encoder.h"

static const struct timespec time_delay = { .tv_nsec = 10 };

typedef struct WebMState {
    AVStream *st;
    int64_t next_pts;
    int64_t serial_samples;
    int samples_count;
    AVFrame *frame;
    AVFrame *tmp_frame;
    struct SwrContext *swr_ctx;
    AVFormatContext *oc;
    AVIOContext *avio_ctx;
    enum packet_flags packet_flags;
    AVCodecContext *c;
} WebMState;


static void avcodec_safe_close(AVCodecContext **c)
{
    while (pthread_mutex_trylock(&g.avc_mutex))
        nanosleep(&time_delay, NULL);
    avcodec_free_context(c);
    pthread_mutex_unlock(&g.avc_mutex);
}


static AVCodec *add_stream(WebMState *self,
                            enum AVCodecID codec_id, int br, int sr, int ch)
{
    AVCodec *codec;
    AVCodecContext *c;

    if (!(codec = avcodec_find_encoder(codec_id))) {
        fprintf(stderr, "could not find encoder for '%s'\n",
                                                avcodec_get_name(codec_id));
        return NULL;
    }

    if (codec->type != AVMEDIA_TYPE_AUDIO) {
        fprintf(stderr, "not an audio codec: %s\n", avcodec_get_name(codec_id));
        return NULL;
    }

    if (!(self->st = avformat_new_stream(self->oc, codec))) {
        fprintf(stderr, "could not allocate stream\n");
        return NULL;
    }

    /* Allocate a codec context for the encoder */
    if (!(c = avcodec_alloc_context3(codec))) {
        fprintf(stderr, "failed to allocate the codec context\n");
        return NULL;
    }
    c->sample_fmt  = codec->sample_fmts ?
                                    codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    c->bit_rate    = br;
    c->sample_rate = sr;
    c->channels    = ch;
    c->channel_layout = (ch == 2) ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
    self->st->id = 0;
    self->st->time_base = (AVRational){ 1, sr };

    /* Copy codec parameters */
    if (avcodec_parameters_from_context(self->st->codecpar, c) < 0) {
        fprintf(stderr, "failed to copy codec parameters to encoder context\n");
        return NULL;
    }

#ifdef AV_CODEC_FLAG_GLOBAL_HEADER
    if (self->oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
#endif

    self->c = c;
    return codec;
}


static AVFrame *alloc_audio_frame(enum AVSampleFormat sample_fmt,
                    uint64_t channel_layout, int sample_rate, int nb_samples)
{
    AVFrame *frame = av_frame_alloc();

    if (!frame) {
        fprintf(stderr, "error allocating an audio frame\n");
        return NULL;
    }

    frame->format = sample_fmt;
    frame->channel_layout = channel_layout;
    frame->sample_rate = sample_rate;
    frame->nb_samples = nb_samples;

    if (nb_samples && av_frame_get_buffer(frame, 0) < 0) {
        fprintf(stderr, "error allocating an audio buffer\n");
        av_frame_free(&frame);
        return NULL;
    }

    return frame;
}


static int open_stream(WebMState *self, AVCodec *codec)
{
    AVCodecContext *c;
    int nb_samples;
    int ret;

    c = self->c;

    while (pthread_mutex_trylock(&g.avc_mutex))
        nanosleep(&time_delay, NULL);
    ret = avcodec_open2(self->c, codec, NULL);
    pthread_mutex_unlock(&g.avc_mutex);

    if (ret < 0) {
        fprintf(stderr, "Could not open audio codec: %s\n", av_err2str(ret));
        return 0;
    }

#ifdef AV_CODEC_CAP_VARIABLE_FRAME_SIZE
    if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        nb_samples = 10000;
    else
        nb_samples = c->frame_size;
#else
    if (!(nb_samples = c->frame_size))
        nb_samples = 10000;
#endif

    self->frame     = alloc_audio_frame(c->sample_fmt, c->channel_layout,
                                       c->sample_rate, nb_samples);
    self->tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_FLTP, c->channel_layout,
                                       c->sample_rate, nb_samples);

    if (!(self->swr_ctx = swr_alloc())) {
        fprintf(stderr, "Could not allocate resampler context\n");
        avcodec_safe_close(&c);
        return 0;
    }

    av_opt_set_int       (self->swr_ctx, "in_channel_count",   c->channels,       0);
    av_opt_set_int       (self->swr_ctx, "in_sample_rate",     c->sample_rate,    0);
    av_opt_set_sample_fmt(self->swr_ctx, "in_sample_fmt",      AV_SAMPLE_FMT_FLTP, 0);
    av_opt_set_int       (self->swr_ctx, "out_channel_count",  c->channels,       0);
    av_opt_set_int       (self->swr_ctx, "out_sample_rate",    c->sample_rate,    0);
    av_opt_set_sample_fmt(self->swr_ctx, "out_sample_fmt",     c->sample_fmt,     0);

    if ((ret = swr_init(self->swr_ctx)) < 0) {
        fprintf(stderr, "Failed to initialize the resampling context\n");
        swr_free(&self->swr_ctx);
        avcodec_safe_close(&c);
        return 0;
    }

    return 1;
}


static AVFrame *get_audio_frame(struct encoder *encoder)
{
    WebMState *self = encoder->encoder_private;
    AVFrame *frame = self->tmp_frame;

    struct encoder_ip_data *id;
    id = encoder_get_input_data(encoder, frame->nb_samples, frame->nb_samples, (float **)frame->data);
    if (id)
    {
        encoder_ip_data_free(id);
        frame->pts = self->next_pts;
        self->next_pts += frame->nb_samples;
        self->serial_samples += frame->nb_samples;
        return frame;
    }

    return NULL;
}


static int write_audio_frame(struct encoder *encoder, int final)
{
    WebMState *self = encoder->encoder_private;
    AVCodecContext *c;
    AVPacket pkt = { 0 };
    AVFrame *frame;
    int ret;
    int got_packet;
    int dst_nb_samples;

    av_init_packet(&pkt);
    c = self->c;
    if (final)
        frame = NULL;
    else {
        go_again:

        if (!(frame = get_audio_frame(encoder)))
            return 0;

        dst_nb_samples = av_rescale_rnd(swr_get_delay(self->swr_ctx, c->sample_rate) + frame->nb_samples,
                                        c->sample_rate, c->sample_rate, AV_ROUND_UP);
        av_assert0(dst_nb_samples == frame->nb_samples);
        if (av_frame_make_writable(self->frame) < 0) {
            fprintf (stderr, "failed to make av frame writable\n");
            return -1;
        }

        if (swr_convert(self->swr_ctx, self->frame->data, dst_nb_samples,
                    (const uint8_t **)frame->data, frame->nb_samples) < 0) {
            fprintf(stderr, "error while converting\n");
            return -1;
        }

        frame = self->frame;
        frame->pts = av_rescale_q(self->samples_count, (AVRational){1, c->sample_rate}, c->time_base);
        self->samples_count += dst_nb_samples;
    }


#ifdef HAVE_AVCODEC_RECEIVE_PACKET
		if ((ret = avcodec_send_frame(c, frame)) < 0) {
			fprintf(stderr, "error encoding audio frame: %s\n", av_err2str(ret));
			return -1;
		}

		for (got_packet = 0; !avcodec_receive_packet(c, &pkt); got_packet = 1)
			if ((ret = av_write_frame(self->oc, &pkt)) < 0) {
				fprintf(stderr, "error while writing audio frame: %s\n", av_err2str(ret));
				return -1;
			}
#else
    if ((ret = avcodec_encode_audio2(c, &pkt, frame, &got_packet)) < 0) {
        fprintf(stderr, "error encoding audio frame: %s\n", av_err2str(ret));
        return -1;
    }

    if (got_packet && (ret = av_write_frame(self->oc, &pkt)) < 0) {
        fprintf(stderr, "error while writing audio frame: %s\n", av_err2str(ret));
        return -1;
    }
#endif

    if (final)
        return 1;

    if (!got_packet)
        goto go_again;

    return 0;
}


static void close_stream(WebMState *self)
{
    avcodec_safe_close(&self->c);
    av_frame_free(&self->frame);
    av_frame_free(&self->tmp_frame);
    swr_free(&self->swr_ctx);
}


static int write_packet(void *opaque, uint8_t *buf, int buf_size)
{
    struct encoder *encoder = opaque;
    WebMState *self = encoder->encoder_private;
    struct encoder_op_packet packet;

    packet.header.bit_rate = encoder->bitrate;
    packet.header.sample_rate = encoder->target_samplerate;
    packet.header.n_channels = encoder->n_channels;
    packet.header.flags = PF_WEBM | self->packet_flags;
    packet.header.data_size = buf_size;
    packet.header.serial = encoder->oggserial;
    packet.header.timestamp = encoder->timestamp = self->serial_samples / (double)encoder->target_samplerate;
    packet.data = buf;
    encoder_write_packet_all(encoder, &packet);
    self->packet_flags &= ~PF_INITIAL;
    return 1;
}


static int write_header(struct encoder *encoder)
{
    WebMState *self = encoder->encoder_private;
    int ret;

    ++encoder->oggserial;
    self->serial_samples = 0;
    self->packet_flags = PF_HEADER | PF_INITIAL;
    ret = avformat_write_header(self->oc, NULL);
    self->packet_flags &= ~PF_HEADER;
    return ret;
}


static int write_trailer(struct encoder *encoder)
{
    WebMState *self = encoder->encoder_private;
    int ret;

    ret = av_write_trailer(self->oc);
    self->packet_flags = PF_FINAL;
    write_packet(encoder, NULL, 0);
    self->packet_flags = 0;
    return ret;
}


static int setup(struct encoder *encoder)
{
    WebMState *self = encoder->encoder_private;
    size_t avio_ctx_buffer_size = 4096;
    AVCodec *codec;
    uint8_t *avio_ctx_buffer;
    enum AVCodecID codec_id;

    switch (encoder->data_format.codec) {
        case ENCODER_CODEC_VORBIS:
            codec_id = AV_CODEC_ID_VORBIS;
            break;
        case ENCODER_CODEC_OPUS:
            codec_id = AV_CODEC_ID_OPUS;
            break;
        default:
            goto fail1;
    }

    if (!(self->oc = avformat_alloc_context())) {
        fprintf(stderr, "avformat_alloc_context failed\n");
        goto fail1;
    }

    if (!(self->oc->oformat = av_guess_format("webm", NULL, "video/webm"))) {
        fprintf(stderr, "format unsupported\n");
        goto fail2;
    }

    if (!(avio_ctx_buffer = av_malloc(avio_ctx_buffer_size))) {
        fprintf(stderr, "av_malloc failed\n");
        goto fail2;
    }

    if (!(self->avio_ctx = avio_alloc_context(avio_ctx_buffer,
            avio_ctx_buffer_size, 1, encoder, NULL, &write_packet, NULL))) {
        fprintf(stderr, "avio_alloc_context failed\n");
        goto fail3;
    }

    self->oc->pb = self->avio_ctx;

    if (!(codec = add_stream(self, codec_id, encoder->bitrate,
                        encoder->target_samplerate, encoder->n_channels))) {
        fprintf(stderr, "failed to add stream\n");
        goto fail4;
    }

    if (!open_stream(self, codec)) {
        fprintf(stderr, "failed to open codec\n");
        goto fail4;
    }

    if (encoder->use_metadata) {
        pthread_mutex_lock(&encoder->metadata_mutex);
        av_dict_set(&self->oc->metadata, "TITLE", encoder->custom_meta, 0);
        pthread_mutex_unlock(&encoder->metadata_mutex);
    }

    if (write_header(encoder) < 0)
        goto fail5;

    return SUCCEEDED;

fail5:
    close_stream(self);
fail4:
    av_freep(&self->avio_ctx->buffer);
    av_freep(&self->avio_ctx);
    goto fail2;
fail3:
    av_freep(&self->avio_ctx->buffer);
fail2:
    avformat_free_context(self->oc);
fail1:
    return FAILED;
}


static void teardown(WebMState *self)
{
    close_stream(self);
    av_freep(&self->avio_ctx->buffer);
    av_freep(&self->avio_ctx);
    avformat_free_context(self->oc);
    memset(self, '\0', sizeof (WebMState));
}


static void live_webm_encoder_main(struct encoder *encoder)
{
    WebMState *self = encoder->encoder_private;

    if (encoder->encoder_state == ES_STARTING)
    {
        if (setup(encoder) == FAILED) {
            goto bailout;
        }
        if (encoder->run_request_f)
            encoder->encoder_state = ES_RUNNING;
        else
            encoder->encoder_state = ES_STOPPING;
        return;
    }

    if (encoder->encoder_state == ES_RUNNING) {
        if (encoder->new_metadata && encoder->use_metadata) {
            encoder->new_metadata = FALSE;
            write_trailer(encoder);
            pthread_mutex_lock(&encoder->metadata_mutex);
            av_dict_set(&self->oc->metadata, "TITLE", encoder->custom_meta, 0);
            pthread_mutex_unlock(&encoder->metadata_mutex);
            write_header(encoder);
        }

        if (encoder->flush) {
            encoder->flush = FALSE;
            write_trailer(encoder);
            write_header(encoder);
        }
        switch (write_audio_frame(encoder, !encoder->run_request_f)) {
            case 0:
                break;
            case -1:
                fprintf(stderr, "error writing out audio frame\n");
            default:
                write_trailer(encoder);
                encoder->encoder_state = ES_STOPPING;
        }
        return;
    }

    if (encoder->encoder_state == ES_STOPPING) {
        teardown(self);
        encoder->flush = FALSE;
        if (encoder->run_request_f) {
            encoder->encoder_state = ES_STARTING;
            return;
        }
    }

bailout:
    fprintf(stderr, "live_webm_encoder_main: performing cleanup\n");
    encoder->run_request_f = FALSE;
    encoder->encoder_state = ES_STOPPED;
    encoder->run_encoder = NULL;
    encoder->flush = FALSE;
    encoder->encoder_private = NULL;
    free(self);
    fprintf(stderr, "live_webm_encoder_main: finished cleanup\n");
}


int live_webm_encoder_init(struct encoder *encoder, struct encoder_vars *ev)
{
    WebMState *self;

    if (!(self = calloc(1, sizeof (WebMState)))) {
        fprintf(stderr, "malloc failure\n");
        return FAILED;
    }

    encoder->encoder_private = self;
    encoder->run_encoder = live_webm_encoder_main;
    return SUCCEEDED;
}

#endif /* HAVE_AVCODEC */
