/* binkw32.dll replacement on FFmpeg: video decoded on demand, audio streamed by a thread into SDL3. */
#include "win32.h"
#include "../d3d8/d3d8_int.h"
#include <SDL3/SDL.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define S(n) void shim_BINKW32_##n(CPU *c)

typedef struct BinkH {
    uint32_t guest;
    char path[1024];
    AVFormatContext *vf; AVCodecContext *vd; int vs; AVFrame *fr; AVPacket *pk; struct SwsContext *sws;
    uint8_t *bgra; int w, h, eof;
    double fps; uint32_t frame, frames;
    uint64_t t0_us, pause_us; int paused;
    pthread_t at; volatile int astop; int has_audio;
    SDL_AudioStream *as; float gain;
} BinkH;

static BinkH *g_bk[64];
static char g_err[256] = "";
static uint32_t g_err_guest;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u; }
static BinkH *bk_of(uint32_t g) { for (int i = 0; i < 64; i++) if (g_bk[i] && g_bk[i]->guest == g) return g_bk[i]; return NULL; }
static void sync_guest(BinkH *b) {
    s32(b->guest + 0, (uint32_t)b->w); s32(b->guest + 4, (uint32_t)b->h); s32(b->guest + 8, b->frames);
    s32(b->guest + 12, b->frame); s32(b->guest + 16, b->frame ? b->frame - 1 : 0);
    s32(b->guest + 20, (uint32_t)(b->fps * 1000 + 0.5)); s32(b->guest + 24, 1000);
}

/* ---------------------------------------------------------------- audio thread */
static void *audio_main(void *arg) {
    BinkH *b = arg;
    AVFormatContext *f = NULL;
    if (avformat_open_input(&f, b->path, NULL, NULL) < 0) return NULL;
    avformat_find_stream_info(f, NULL);
    int si = av_find_best_stream(f, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (si < 0) { avformat_close_input(&f); return NULL; }
    const AVCodec *codec = avcodec_find_decoder(f->streams[si]->codecpar->codec_id);
    AVCodecContext *d = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(d, f->streams[si]->codecpar);
    if (avcodec_open2(d, codec, NULL) < 0) { avcodec_free_context(&d); avformat_close_input(&f); return NULL; }
    SwrContext *swr = NULL;
    AVChannelLayout out_l = AV_CHANNEL_LAYOUT_STEREO;
    swr_alloc_set_opts2(&swr, &out_l, AV_SAMPLE_FMT_FLT, 44100, &d->ch_layout, d->sample_fmt, d->sample_rate, 0, NULL);
    swr_init(swr);
    AVPacket *pk = av_packet_alloc(); AVFrame *fr = av_frame_alloc();
    float *buf = NULL; int cap = 0;
    while (!b->astop) {
        while (!b->astop && SDL_GetAudioStreamQueued(b->as) > 44100 * 8 / 4) usleep(10000);   /* keep ~250 ms queued */
        if (av_read_frame(f, pk) < 0) break;
        if (pk->stream_index == si && avcodec_send_packet(d, pk) >= 0) {
            while (avcodec_receive_frame(d, fr) >= 0) {
                int n = swr_get_out_samples(swr, fr->nb_samples);
                if (n * 2 > cap) { cap = n * 2; buf = realloc(buf, sizeof(float) * (size_t)cap); }
                uint8_t *outp = (uint8_t *)buf;
                int got = swr_convert(swr, &outp, n, (const uint8_t **)fr->extended_data, fr->nb_samples);
                if (got > 0) SDL_PutAudioStreamData(b->as, buf, got * 2 * (int)sizeof(float));
            }
        }
        av_packet_unref(pk);
    }
    free(buf); av_frame_free(&fr); av_packet_free(&pk); swr_free(&swr);
    avcodec_free_context(&d); avformat_close_input(&f);
    return NULL;
}

/* ---------------------------------------------------------------- video */
static int decode_next(BinkH *b) {
    while (!b->eof) {
        int r = avcodec_receive_frame(b->vd, b->fr);
        if (r == 0) return 1;
        if (r != AVERROR(EAGAIN)) return 0;
        if (av_read_frame(b->vf, b->pk) < 0) { b->eof = 1; avcodec_send_packet(b->vd, NULL); continue; }
        if (b->pk->stream_index == b->vs) avcodec_send_packet(b->vd, b->pk);
        av_packet_unref(b->pk);
    }
    return avcodec_receive_frame(b->vd, b->fr) == 0;
}

S(_BinkOpen_8) {
    uint32_t arg = ARG(c, 0), flags = ARG(c, 1);
    BinkH *b = calloc(1, sizeof *b);
    const char *name;
    if (flags & 0x00800000u) {                    /* BINKFILEHANDLE: arg is a Win32 file handle */
        const char *hp = file_handle_path(arg);
        LOG("[bink] open by handle %08X -> %s\n", arg, hp ? hp : "?");
        if (!hp) { free(b); snprintf(g_err, sizeof g_err, "bad handle"); RET(c, 0, 2); return; }
        snprintf(b->path, sizeof b->path, "%s", hp);
        name = b->path;
    } else {
        name = gstr(arg);
        if (!name) { LOG("[bink] open NULL name flags=%08X\n", flags); free(b); RET(c, 0, 2); return; }
        host_path(name, b->path, sizeof b->path, 1);
    }
    if (avformat_open_input(&b->vf, b->path, NULL, NULL) < 0) {
        snprintf(g_err, sizeof g_err, "Bink: cannot open %s", name);
        LOG("[bink] open %s FAILED\n", name);
        free(b); RET(c, 0, 2); return;
    }
    avformat_find_stream_info(b->vf, NULL);
    b->vs = av_find_best_stream(b->vf, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (b->vs >= 0) {
        AVStream *st = b->vf->streams[b->vs];
        const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
        b->vd = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(b->vd, st->codecpar);
        avcodec_open2(b->vd, codec, NULL);
        b->w = st->codecpar->width; b->h = st->codecpar->height;
        b->fps = st->avg_frame_rate.den ? av_q2d(st->avg_frame_rate) : (st->r_frame_rate.den ? av_q2d(st->r_frame_rate) : 30);
        b->frames = st->nb_frames > 0 ? (uint32_t)st->nb_frames : 0;
        if (!b->frames) {   /* Bink header: 'BIK?' size frames ... */
            FILE *fh = fopen(b->path, "rb");
            uint8_t hdr[12] = {0};
            if (fh) { fread(hdr, 1, 12, fh); fclose(fh); }
            b->frames = (uint32_t)hdr[8] | (uint32_t)hdr[9] << 8 | (uint32_t)hdr[10] << 16 | (uint32_t)hdr[11] << 24;
            if (!b->frames) b->frames = 1;
        }
        b->bgra = calloc((size_t)b->w * (size_t)b->h, 4);
    }
    b->fr = av_frame_alloc(); b->pk = av_packet_alloc();
    b->frame = 1; b->gain = 1;
    b->has_audio = av_find_best_stream(b->vf, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0) >= 0;
    b->guest = galloc(512, 1);
    sync_guest(b);
    for (int i = 0; i < 64; i++) if (!g_bk[i]) { g_bk[i] = b; break; }
    if (b->has_audio) {
        SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, 44100};
        b->as = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (b->as) {
            if (getenv("QUIET")) SDL_SetAudioStreamGain(b->as, 0.f);
            SDL_ResumeAudioStreamDevice(b->as);
            pthread_create(&b->at, NULL, audio_main, b);
        }
    }
    b->t0_us = now_us();
    LOG("[bink] open %s: %dx%d %.2f fps %u frames%s\n", name, b->w, b->h, b->fps, b->frames, b->has_audio ? " +audio" : "");
    RET(c, b->guest, 2);
}
S(_BinkClose_4) {
    BinkH *b = bk_of(ARG(c, 0));
    if (b) {
        b->astop = 1;
        if (b->as) { pthread_join(b->at, NULL); SDL_DestroyAudioStream(b->as); }
        if (b->vd) avcodec_free_context(&b->vd);
        if (b->vf) avformat_close_input(&b->vf);
        if (b->sws) sws_freeContext(b->sws);
        av_frame_free(&b->fr); av_packet_free(&b->pk);
        free(b->bgra); gfree(b->guest);
        for (int i = 0; i < 64; i++) if (g_bk[i] == b) g_bk[i] = NULL;
        free(b);
    }
    RET(c, 0, 1);
}
S(_BinkDoFrame_4) {
    BinkH *b = bk_of(ARG(c, 0));
    if (b && b->vd && decode_next(b)) {
        b->sws = sws_getCachedContext(b->sws, b->fr->width, b->fr->height, b->fr->format, b->w, b->h, AV_PIX_FMT_BGRA,
                                      SWS_BILINEAR, NULL, NULL, NULL);
        uint8_t *dst[1] = {b->bgra}; int ls[1] = {b->w * 4};
        sws_scale(b->sws, (const uint8_t *const *)b->fr->data, b->fr->linesize, 0, b->fr->height, dst, ls);
    }
    RET(c, 0, 1);
}
S(_BinkNextFrame_4) {
    BinkH *b = bk_of(ARG(c, 0));
    if (b) { if (b->frame < b->frames) b->frame++; sync_guest(b); }
    RET(c, 0, 1);
}
S(_BinkWait_4) {
    BinkH *b = bk_of(ARG(c, 0));
    if (!b || b->paused) { RET(c, 1, 1); return; }
    double due = (b->frame - 1) / b->fps;
    double t = (now_us() - b->t0_us) / 1e6;
    if (t < due) usleep(1000);   /* avoid busy spinning in the caller's wait loop */
    RET(c, t < due ? 1u : 0u, 1);
}
S(_BinkPause_8) {
    BinkH *b = bk_of(ARG(c, 0));
    uint32_t p = ARG(c, 1);
    if (b && p != (uint32_t)b->paused) {
        if (p) { b->pause_us = now_us(); if (b->as) SDL_PauseAudioStreamDevice(b->as); }
        else { b->t0_us += now_us() - b->pause_us; if (b->as) SDL_ResumeAudioStreamDevice(b->as); }
        b->paused = (int)p;
    }
    RET(c, p, 2);
}
S(_BinkSetVolume_12) {
    BinkH *b = bk_of(ARG(c, 0));
    if (b && b->as && !getenv("QUIET")) SDL_SetAudioStreamGain(b->as, (float)ARG(c, 2) / 32768.f);
    RET(c, 0, 3);
}
S(_BinkCopyToBuffer_28) {
    BinkH *b = bk_of(ARG(c, 0));
    uint32_t dst = ARG(c, 1), pitch = ARG(c, 2), dh = ARG(c, 3), dx = ARG(c, 4), dy = ARG(c, 5), type = ARG(c, 6) & 15;
    if (!b || !b->bgra) { RET(c, 0, 7); return; }
    for (int y = 0; y < b->h && (uint32_t)y + dy < dh; y++) {
        const uint8_t *s = b->bgra + (size_t)y * (size_t)b->w * 4;
        uint8_t *d = gptr(dst + (dy + (uint32_t)y) * pitch);
        if (type == 3 || type == 5) { memcpy(d + dx * 4, s, (size_t)b->w * 4); continue; }
        for (int x = 0; x < b->w; x++) {
            uint32_t B = s[4 * x], G = s[4 * x + 1], R = s[4 * x + 2];
            uint16_t v;
            if (type == 10) v = (uint16_t)((R >> 3) << 11 | (G >> 2) << 5 | B >> 3);
            else if (type == 7) v = (uint16_t)(0xF000 | (R >> 4) << 8 | (G >> 4) << 4 | B >> 4);
            else v = (uint16_t)(0x8000 | (R >> 3) << 10 | (G >> 3) << 5 | B >> 3);
            memcpy(d + (dx + (uint32_t)x) * 2, &v, 2);
        }
    }
    RET(c, 0, 7);
}
S(_BinkDX8SurfaceType_4) {
    Surf *s = com_host(ARG(c, 0));
    uint32_t f = s ? s->fmt : D3DFMT_X8R8G8B8;
    uint32_t t = f == D3DFMT_A8R8G8B8 ? 5 : f == D3DFMT_R5G6B5 ? 10 : f == D3DFMT_A4R4G4B4 ? 7 : f == D3DFMT_A1R5G5B5 ? 8 :
                 f == D3DFMT_X1R5G5B5 ? 9 : 3;
    RET(c, t, 1);
}
S(_BinkSetSoundSystem_8) { RET(c, 1, 2); }
static void dummy_sound_open(CPU *c) { RET(c, 1, 1); }
S(_BinkOpenDirectSound_4) {
    static uint32_t thunk;
    if (!thunk) thunk = rt_register_thunk(dummy_sound_open, "bink:sound_open");
    RET(c, thunk, 1);
}
S(_BinkSetSoundTrack_8) { RET(c, 0, 2); }
S(_BinkSetVideoOnOff_8) { RET(c, ARG(c, 1), 2); }
S(_BinkLogoAddress_0) { RET(c, 0, 0); }
S(_BinkGetError_0) {
    if (!g_err_guest) g_err_guest = galloc(256, 1);
    snprintf((char *)gptr(g_err_guest), 256, "%s", g_err);
    RET(c, g_err_guest, 0);
}
