#include "ffmpeg_stream.h"
#include "infra/log.h"
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

struct FFStream {
    AVFormatContext *fmt;
    AVCodecContext  *codec;
    SwrContext      *swr;
    AVPacket        *pkt;
    AVFrame         *frm;
    int stream_idx, channels, sample_rate;
    int eof, flushing;
};

FFStream* ffstream_open(const char *url, int *sr, int *ch, int *dur) {
    av_log_set_level(AV_LOG_ERROR);
    FFStream *s = calloc(1, sizeof(FFStream));
    if (!s) return NULL;

    if (avformat_open_input(&s->fmt, url, NULL, NULL) < 0) goto fail;
    s->fmt->max_analyze_duration = 10 * AV_TIME_BASE;
    if (avformat_find_stream_info(s->fmt, NULL) < 0) goto fail;

    const AVCodec *codec = NULL;
    s->stream_idx = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1,-1,&codec,0);
    if (s->stream_idx < 0) goto fail;

    AVCodecParameters *par = s->fmt->streams[s->stream_idx]->codecpar;
    s->codec = avcodec_alloc_context3(codec);
    if (!s->codec) goto fail;
    if (avcodec_parameters_to_context(s->codec, par) < 0) goto fail;
    if (avcodec_open2(s->codec, codec, NULL) < 0) goto fail;

    s->sample_rate = s->codec->sample_rate;
    s->channels    = s->codec->ch_layout.nb_channels;

    AVChannelLayout out_l = s->codec->ch_layout;
    swr_alloc_set_opts2(&s->swr,
        &out_l, AV_SAMPLE_FMT_S16, s->sample_rate,
        &s->codec->ch_layout, s->codec->sample_fmt, s->sample_rate, 0, NULL);
    if (!s->swr || swr_init(s->swr) < 0) goto fail;

    s->pkt = av_packet_alloc();
    s->frm = av_frame_alloc();
    if (!s->pkt || !s->frm) goto fail;

    if (sr)  *sr  = s->sample_rate;
    if (ch)  *ch  = s->channels;
    if (dur) *dur = (int)(s->fmt->duration / AV_TIME_BASE);

    return s;

fail:
    ffstream_close(s);
    return NULL;
}

int ffstream_decode(FFStream *s, int16_t *pcm, int max_frames) {
    if (!s || s->eof) return 0;

    while (1) {
        int ret = avcodec_receive_frame(s->codec, s->frm);
        if (ret == 0) {
            /* got a frame — resample to S16 interleaved */
            int out_samples = s->frm->nb_samples;
            if (out_samples > max_frames) out_samples = max_frames;
            uint8_t *out[1] = {(uint8_t*)pcm};
            int n = swr_convert(s->swr, out, out_samples,
                                (const uint8_t**)s->frm->data, s->frm->nb_samples);
            av_frame_unref(s->frm);
            return n > 0 ? n : 0;
        }
        if (ret == AVERROR(EAGAIN)) {
            /* need more input — read a packet */
            if (s->flushing) { s->eof = 1; return 0; }
            int r = av_read_frame(s->fmt, s->pkt);
            if (r < 0) {
                avcodec_send_packet(s->codec, NULL);
                s->flushing = 1;
                continue;
            }
            if (s->pkt->stream_index == s->stream_idx)
                avcodec_send_packet(s->codec, s->pkt);
            av_packet_unref(s->pkt);
            continue;
        }
        /* EOF or error */
        s->eof = 1; return 0;
    }
}

void ffstream_close(FFStream *s) {
    if (!s) return;
    av_frame_free(&s->frm);
    av_packet_free(&s->pkt);
    swr_free(&s->swr);
    avcodec_free_context(&s->codec);
    avformat_close_input(&s->fmt);
    free(s);
}
