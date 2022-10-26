/*
#   live_aac_encoder.c: encode using libavformat
#   Copyright (C) 2015-2022 Stephen Fairchild (s-fairchild@users.sf.net)
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
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include "main.h"
#include "sourceclient.h"
#include "live_aac_encoder.h"

static const struct timespec time_delay = { .tv_nsec = 10 };

typedef struct State {
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
    enum packet_flags const_packet_flags;
    AVCodecContext *c;
    char *metadata;
    uint8_t *buf;
    size_t buf_size;
    int sri;
    AVPacket *pkt;
} State;


static void avcodec_safe_close(AVCodecContext **c)
{
    while (pthread_mutex_trylock(&g.avc_mutex))
        nanosleep(&time_delay, NULL);
    avcodec_free_context(c);
    pthread_mutex_unlock(&g.avc_mutex);
}


static const AVCodec *add_stream(State *self, enum AVCodecID codec_id,
                           int profile, int br, int sr, int ch)
{
    AVCodecContext *c;
    const AVCodec *codec = avcodec_find_encoder(codec_id);

    if (!codec) {
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
    c->profile = profile;
    self->st->id = 0;
    self->st->time_base = (AVRational){ 1, sr };

    /* Copy codec parameters */
    if (avcodec_parameters_from_context(self->st->codecpar, c) < 0) {
        fprintf(stderr, "failed to copy codec parameters to encoder context\n");
        return NULL;
    }

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


static int open_stream(State *self, const AVCodec *codec)
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
    State *self = encoder->encoder_private;
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
    State *self = encoder->encoder_private;
    AVCodecContext *c;
    AVFrame *frame;
    int ret;
    int got_packet;
    int dst_nb_samples;

    if (self->pkt)
        av_packet_unref(self->pkt);
    if (!(self->pkt = av_packet_alloc())) {
        fprintf(stderr, "av_packet_init failed\n");
        return -1;
    }
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

        for (got_packet = 0; !avcodec_receive_packet(c, self->pkt); got_packet = 1)
            if ((ret = av_write_frame(self->oc, self->pkt)) < 0) {
                fprintf(stderr, "error while writing audio frame: %s\n", av_err2str(ret));
                return -1;
            }
#else
    if ((ret = avcodec_encode_audio2(c, self->pkt, frame, &got_packet)) < 0) {
        fprintf(stderr, "error encoding audio frame: %s\n", av_err2str(ret));
        return -1;
    }

    if (got_packet && (ret = av_write_frame(self->oc, self->pkt)) < 0) {
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


static void close_stream(State *self)
{
    if (self->pkt)
        av_packet_unref(self->pkt);
    avcodec_safe_close(&self->c);
    av_frame_free(&self->frame);
    av_frame_free(&self->tmp_frame);
    swr_free(&self->swr_ctx);
}

static void packetize_metadata(struct encoder *e, State * const s)
    {
    size_t l = 4;

    pthread_mutex_lock(&e->metadata_mutex);

    l += strlen(e->custom_meta);
    l += strlen(e->artist);
    l += strlen(e->title);
    l += strlen(e->album);

    if ((s->metadata = realloc(s->metadata, l)))
        snprintf(s->metadata, l, "%s\n%s\n%s\n%s", e->custom_meta, e->artist, e->title, e->album);
    else
        fprintf(stderr, "malloc failure\n");

    e->new_metadata = FALSE;
    pthread_mutex_unlock(&e->metadata_mutex);
    }

static int write_packet(struct encoder *encoder, uint8_t *buf, int buf_size, enum packet_flags pf)
{
    State *self = encoder->encoder_private;
    struct encoder_op_packet packet;

    packet.header.bit_rate = encoder->bitrate;
    packet.header.sample_rate = encoder->target_samplerate;
    packet.header.n_channels = encoder->n_channels;
    packet.header.flags = self->const_packet_flags | self->packet_flags | pf;
    packet.header.data_size = buf_size;
    packet.header.serial = encoder->oggserial;
    packet.header.timestamp = encoder->timestamp = self->serial_samples / (double)encoder->target_samplerate;
    packet.data = buf;
    encoder_write_packet_all(encoder, &packet);
    self->packet_flags &= ~PF_INITIAL;

    return 1;
}

static int write_packet_wrapper(void *opaque, uint8_t *buf, int buf_size)
{
    struct encoder *encoder = (struct encoder *)opaque;
    State *self = encoder->encoder_private;

    if (7 + buf_size > self->buf_size)
        if (!(self->buf = realloc(self->buf, self->buf_size = 7 + buf_size))) {
            fprintf(stderr, "malloc failure\n");
            exit(5);
        }

    {   // ADTS header - n.b. buffer fullness set to all ones as per the FAAC encoder's behaviour
        uint8_t *b = self->buf;
        *b++ = 0xff;    // syncword
        *b++ = 0xf1;    // syncword, mpeg 4, layer 0, unprotected
        *b++ = 0x40 | self->sri;
        int s = 7 + buf_size;
        *b++ = encoder->n_channels << 6 | s >> 11;
        *b++ = (s >> 3) & 0xFF;
        *b++ = ((s << 5) & 0xFF) | 0x1f;
        *b++ = 0xFC;
    }

    memcpy(self->buf + 7, buf, buf_size);
    return write_packet(encoder, self->buf, 7 + buf_size, 0);
}

static int write_header(struct encoder *encoder)
{
    State *self = encoder->encoder_private;
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
    State *self = encoder->encoder_private;
    int ret;

    ret = av_write_trailer(self->oc);
    self->packet_flags = PF_FINAL;
    write_packet(encoder, NULL, 0, 0);
    self->packet_flags = 0;
    return ret;
}


static int setup(struct encoder *encoder)
{
    State *self = encoder->encoder_private;
    size_t avio_ctx_buffer_size = 4096;
    uint8_t *avio_ctx_buffer;
    enum AVCodecID codec_id;
    int profile;

    switch (encoder->data_format.codec) {
        case ENCODER_CODEC_AAC:
            self->const_packet_flags = PF_AAC;
            codec_id = AV_CODEC_ID_AAC;
            profile = FF_PROFILE_AAC_LOW;
            break;
        case ENCODER_CODEC_AACPLUSV2:
            self->const_packet_flags = PF_AACP2;
            codec_id = AV_CODEC_ID_AAC;
            profile = FF_PROFILE_AAC_HE_V2;
            break;
        default:
            goto fail1;
    }

    if (!(self->oc = avformat_alloc_context())) {
        fprintf(stderr, "avformat_alloc_context failed\n");
        goto fail1;
    }

    if (!(self->oc->oformat = av_guess_format("adts", NULL, NULL))) {
        fprintf(stderr, "format unsupported\n");
        goto fail2;
    }

    if (!(avio_ctx_buffer = av_malloc(avio_ctx_buffer_size))) {
        fprintf(stderr, "av_malloc failed\n");
        goto fail2;
    }

    if (!(self->avio_ctx = avio_alloc_context(avio_ctx_buffer,
            avio_ctx_buffer_size, 1, encoder, NULL, &write_packet_wrapper, NULL))) {
        fprintf(stderr, "avio_alloc_context failed\n");
        goto fail3;
    }

    self->oc->pb = self->avio_ctx;

    const AVCodec *codec = add_stream(self, codec_id, profile,
                                      encoder->bitrate,
                                      encoder->target_samplerate,
                                      encoder->n_channels);
    if (!codec) {
        fprintf(stderr, "failed to add stream\n");
        goto fail4;
    }

    if (!open_stream(self, codec)) {
        fprintf(stderr, "failed to open codec\n");
        goto fail4;
    }

    if (write_header(encoder) < 0)
        goto fail5;

    int sri;
    switch (encoder->target_samplerate) {
        case 96000:
            sri = 0;
            break;
        case 88200:
            sri = 1;
            break;
        case 64000:
            sri = 2;
            break;
        case 48000:
            sri = 3;
            break;
        case 44100:
            sri = 4;
            break;
        case 32000:
            sri = 5;
            break;
        case 24000:
            sri = 6;
            break;
        case 22050:
            sri = 7;
            break;
        case 16000:
            sri = 8;
            break;
        case 12000:
            sri = 9;
            break;
        case 11025:
            sri = 10;
            break;
        case 8000:
            sri = 11;
            break;
        case 7350:
            sri = 12;
            break;
        default:
            fprintf(stderr, "live_aac_encoder.c: bad sample rate index\n");
            goto fail5;
    }
    self->sri = sri << 2;

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


static void teardown(State *self)
{
    close_stream(self);
    av_freep(&self->avio_ctx->buffer);
    av_freep(&self->avio_ctx);
    avformat_free_context(self->oc);
    if (self->metadata)
        free(self->metadata);
    if (self->buf)
        free(self->buf);
    memset(self, '\0', sizeof (State));
}


static void live_aac_encoder_main(struct encoder *encoder)
{
    State *self = encoder->encoder_private;

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
        if (encoder->new_metadata && encoder->use_metadata && !(self->packet_flags & (PF_INITIAL | PF_FINAL))) {
            packetize_metadata(encoder, self);
            if (self->metadata)
                write_packet(encoder, (unsigned char *)self->metadata, strlen(self->metadata) + 1, PF_METADATA);
        }

        switch (write_audio_frame(encoder, !encoder->run_request_f || encoder->flush)) {
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
    fprintf(stderr, "live_aac_encoder_main: performing cleanup\n");
    encoder->run_request_f = FALSE;
    encoder->encoder_state = ES_STOPPED;
    encoder->run_encoder = NULL;
    encoder->flush = FALSE;
    encoder->encoder_private = NULL;
    free(self);
    fprintf(stderr, "live_aac_encoder_main: finished cleanup\n");
}


int live_aac_encoder_init(struct encoder *encoder, struct encoder_vars *ev)
{
    State *self;

    if (!(self = calloc(1, sizeof (State)))) {
        fprintf(stderr, "malloc failure\n");
        return FAILED;
    }

    encoder->encoder_private = self;
    encoder->run_encoder = live_aac_encoder_main;
    return SUCCEEDED;
}

#endif /* HAVE_AVCODEC */
