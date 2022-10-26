/*
#   avcodecdecode.h: decodes wma file format for xlplayer
#   Copyright (C) 2007, 2011 Stephen Fairchild (s-fairchild@users.sourceforge.net)
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

#ifdef HAVE_LIBAV

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#ifdef HAVE_SWRESAMPLE
#include <libswresample/swresample.h>
#endif

#include "xlplayer.h"
#include "mp3tagread.h"

struct avcodecdecode_vars
    {
    AVCodec *codec;
    AVPacket pkt;
    AVPacket pktcopy;
    AVCodecContext *c;
    AVFormatContext *ic;
    int size;
    int resample;
    unsigned int stream;
#ifdef HAVE_AVCODEC_SEND_PACKET
    AVFrame af;
#else
    AVFrame *frame;
#endif
    float drop;
    struct mp3taginfo taginfo;
    struct chapter *current_chapter;
    int channels;   /* number of downmixed channels 1 or 2 */

#ifdef HAVE_SWRESAMPLE
    SwrContext *swr;
    uint8_t *floatsamples;
#else
    float *floatsamples;
#endif

    };

int avcodecdecode_reg(struct xlplayer *xlplayer);

#endif /* HAVE_LIBAV */
