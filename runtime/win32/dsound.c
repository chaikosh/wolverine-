/* DirectSound 8: software mixer over SDL3 audio. Buffers live in guest memory. */
#include "../com.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>

#define OUT_RATE 44100
#define MAXBUF 512

typedef struct SBuf {
    uint32_t obj, obj3d, obj8;
    int primary, playing, looping, is3d;
    uint32_t mem, size;          /* guest ring buffer */
    uint32_t channels, bits, rate, freq;
    double pos;                  /* play cursor in frames (fractional) */
    int32_t volume, pan;         /* hundredths of dB */
    float p3[3], mind, maxd;     /* 3D */
    uint32_t mode3d;
} SBuf;

static SBuf *g_bufs[MAXBUF];
static pthread_mutex_t g_mix = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioStream *g_stream;
static uint32_t vt_ds, vt_buf, vt_3d, vt_lis, g_ds_obj;
static float g_lis_pos[3], g_lis_front[3] = {0, 0, 1}, g_lis_top[3] = {0, 1, 0}, g_rolloff = 1, g_distf = 1;

static inline float db2lin(int32_t v) { return v <= -10000 ? 0.f : powf(10.f, (float)v / 2000.f); }

static void mix_cb(void *ud, SDL_AudioStream *st, int additional, int total) {
    (void)ud; (void)total;
    int frames = additional / (int)(2 * sizeof(float));
    if (frames <= 0) return;
    float *out = calloc((size_t)frames * 2, sizeof(float));
    pthread_mutex_lock(&g_mix);
    /* listener basis for 3D: right = top x front */
    float *F = g_lis_front, *T = g_lis_top;
    float R[3] = {T[1] * F[2] - T[2] * F[1], T[2] * F[0] - T[0] * F[2], T[0] * F[1] - T[1] * F[0]};
    float rl = sqrtf(R[0] * R[0] + R[1] * R[1] + R[2] * R[2]); if (rl > 0) for (int k = 0; k < 3; k++) R[k] /= rl;
    for (int b = 0; b < MAXBUF; b++) {
        SBuf *s = g_bufs[b];
        if (!s || !s->playing || s->primary || !s->mem || !s->size) continue;
        uint32_t bpf = s->channels * s->bits / 8, nfr = s->size / bpf;
        if (!bpf || !nfr) continue;
        float vol = db2lin(s->volume), pl = 1, pr = 1;
        if (s->pan > 0) pl = db2lin(-s->pan); else if (s->pan < 0) pr = db2lin(s->pan);
        if (s->is3d && s->mode3d != 2) {   /* DS3DMODE_DISABLE == 2 */
            float d[3] = {s->p3[0], s->p3[1], s->p3[2]};
            if (s->mode3d == 0) for (int k = 0; k < 3; k++) d[k] -= g_lis_pos[k];   /* NORMAL: world space */
            float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * g_distf;
            float md = s->mind > 0 ? s->mind : 1;
            if (dist > s->maxd && s->maxd > 0) dist = s->maxd;
            float att = dist <= md ? 1.f : md / (md + g_rolloff * (dist - md));
            float side = dist > 1e-4f ? (d[0] * R[0] + d[1] * R[1] + d[2] * R[2]) / (dist / g_distf) : 0;
            vol *= att;
            pl *= side > 0 ? 1 - 0.6f * side : 1; pr *= side < 0 ? 1 + 0.6f * side : 1;
        }
        double step = (double)(s->freq ? s->freq : s->rate) / OUT_RATE;
        const uint8_t *m = gptr(s->mem);
        for (int i = 0; i < frames; i++) {
            uint32_t fi = (uint32_t)s->pos;
            if (fi >= nfr) {
                if (s->looping) { s->pos -= nfr; fi = (uint32_t)s->pos; }
                else { s->playing = 0; s->pos = 0; break; }
            }
            float l, r;
            const uint8_t *p = m + fi * bpf;
            if (s->bits == 16) {
                int16_t a, b2; memcpy(&a, p, 2);
                if (s->channels == 2) memcpy(&b2, p + 2, 2); else b2 = a;
                l = a / 32768.f; r = b2 / 32768.f;
            } else {
                l = (p[0] - 128) / 128.f; r = s->channels == 2 ? (p[1] - 128) / 128.f : l;
            }
            out[2 * i] += l * vol * pl; out[2 * i + 1] += r * vol * pr;
            s->pos += step;
        }
    }
    pthread_mutex_unlock(&g_mix);
    static int quiet = -1;
    if (quiet < 0) quiet = getenv("QUIET") != NULL;   /* test runs: keep timing, output silence */
    for (int i = 0; i < frames * 2; i++) out[i] = quiet ? 0.f : out[i] > 1 ? 1 : out[i] < -1 ? -1 : out[i];
    SDL_PutAudioStreamData(st, out, frames * 2 * (int)sizeof(float));
    free(out);
}

static void audio_start(void) {
    if (g_stream) return;
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, OUT_RATE};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, mix_cb, NULL);
    if (!g_stream) { LOG("[dsound] SDL audio open failed: %s\n", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(g_stream);
    LOG("[dsound] mixer running at %d Hz\n", OUT_RATE);
}

#define M(n) static void n(CPU *c)
#define BUF ((SBuf *)com_host(THIS))
M(AddRef) { RET(c, com_addref(THIS), 1); }

/* ---------------------------------------------------------------- buffers */
static SBuf *new_buf(void) {
    SBuf *s = calloc(1, sizeof *s);
    s->mind = 1; s->maxd = 1e9f;
    pthread_mutex_lock(&g_mix);
    for (int i = 0; i < MAXBUF; i++) if (!g_bufs[i]) { g_bufs[i] = s; break; }
    pthread_mutex_unlock(&g_mix);
    return s;
}
static void free_buf(SBuf *s) {
    pthread_mutex_lock(&g_mix);
    for (int i = 0; i < MAXBUF; i++) if (g_bufs[i] == s) g_bufs[i] = NULL;
    pthread_mutex_unlock(&g_mix);
    if (s->mem) gfree(s->mem);
    free(s);
}
M(B_QueryInterface) {
    SBuf *s = BUF; uint32_t id = g32(ARG(c, 1)), out = ARG(c, 2);
    if (id == 0x279AFA86u) { s->is3d = 1; if (!s->obj3d) s->obj3d = com_new(vt_3d, s); s32(out, s->obj3d); RET(c, HR_OK, 3); }
    else if (id == 0x279AFA84u) { if (!s->obj3d) s->obj3d = com_new(vt_lis, s); s32(out, s->obj3d); RET(c, HR_OK, 3); }
    else if (id == 0x6825A449u || id == 0x279AFA85u) { com_addref(THIS); s32(out, THIS); RET(c, HR_OK, 3); }
    else { s32(out, 0); RET(c, E_NOINTERFACE, 3); }
}
M(B_Release) {
    uint32_t r = com_release(THIS);
    if (!r && !BUF->primary) { pthread_mutex_lock(&g_mix); BUF->playing = 0; pthread_mutex_unlock(&g_mix); }
    RET(c, r, 1);
}
M(B_GetCaps) { SBuf *s = BUF; uint32_t p = ARG(c, 1); s32(p + 4, 0x8 | 0x80); s32(p + 8, s->size); RET(c, HR_OK, 2); }
M(B_GetCurrentPosition) {
    SBuf *s = BUF;
    pthread_mutex_lock(&g_mix);
    uint32_t bpf = s->channels * s->bits / 8;
    uint32_t play = bpf ? ((uint32_t)s->pos * bpf) % (s->size ? s->size : 1) : 0;
    pthread_mutex_unlock(&g_mix);
    uint32_t ahead = (s->freq ? s->freq : s->rate) / 50 * bpf;   /* ~20 ms safety */
    if (ARG(c, 1)) s32(ARG(c, 1), play);
    if (ARG(c, 2)) s32(ARG(c, 2), s->size ? (play + ahead) % s->size : 0);
    RET(c, HR_OK, 3);
}
M(B_GetFormat) {
    SBuf *s = BUF; uint32_t p = ARG(c, 1);
    if (p) { s16(p, 1); s16(p + 2, (uint16_t)s->channels); s32(p + 4, s->rate); s32(p + 8, s->rate * s->channels * s->bits / 8);
             s16(p + 12, (uint16_t)(s->channels * s->bits / 8)); s16(p + 14, (uint16_t)s->bits); s16(p + 16, 0); }
    if (ARG(c, 3)) s32(ARG(c, 3), 18);
    RET(c, HR_OK, 4);
}
M(B_GetVolume) { s32(ARG(c, 1), (uint32_t)BUF->volume); RET(c, HR_OK, 2); }
M(B_GetPan) { s32(ARG(c, 1), (uint32_t)BUF->pan); RET(c, HR_OK, 2); }
M(B_GetFrequency) { s32(ARG(c, 1), BUF->freq ? BUF->freq : BUF->rate); RET(c, HR_OK, 2); }
M(B_GetStatus) { SBuf *s = BUF; s32(ARG(c, 1), s->playing ? (1u | (s->looping ? 4u : 0u)) : 0u); RET(c, HR_OK, 2); }
M(B_Lock) {
    SBuf *s = BUF;
    uint32_t off = ARG(c, 1), n = ARG(c, 2), flags = ARG(c, 7);
    if (!s->size) { RET(c, E_FAIL, 8); return; }
    if (flags & 2) { off = 0; n = s->size; }                   /* DSBLOCK_ENTIREBUFFER */
    if (flags & 1) {                                           /* DSBLOCK_FROMWRITECURSOR */
        uint32_t bpf = s->channels * s->bits / 8;
        off = bpf ? ((uint32_t)s->pos * bpf) % s->size : 0;
    }
    off %= s->size;
    if (n > s->size) n = s->size;
    uint32_t n1 = n, n2 = 0;
    if (off + n > s->size) { n1 = s->size - off; n2 = n - n1; }
    s32(ARG(c, 3), s->mem + off); s32(ARG(c, 4), n1);
    if (ARG(c, 5)) s32(ARG(c, 5), n2 ? s->mem : 0);
    if (ARG(c, 6)) s32(ARG(c, 6), n2);
    RET(c, HR_OK, 8);
}
M(B_Unlock) { RET(c, HR_OK, 5); }
M(B_Play) {
    SBuf *s = BUF;
    pthread_mutex_lock(&g_mix);
    s->playing = 1; s->looping = (ARG(c, 3) & 1) != 0;
    pthread_mutex_unlock(&g_mix);
    RET(c, HR_OK, 4);
}
M(B_SetCurrentPosition) {
    SBuf *s = BUF; uint32_t bpf = s->channels * s->bits / 8;
    pthread_mutex_lock(&g_mix);
    s->pos = bpf ? (double)(ARG(c, 1) / bpf) : 0;
    pthread_mutex_unlock(&g_mix);
    RET(c, HR_OK, 2);
}
M(B_SetFormat) { RET(c, HR_OK, 2); }
M(B_SetVolume) { BUF->volume = (int32_t)ARG(c, 1); RET(c, HR_OK, 2); }
M(B_SetPan) { BUF->pan = (int32_t)ARG(c, 1); RET(c, HR_OK, 2); }
M(B_SetFrequency) { BUF->freq = ARG(c, 1); RET(c, HR_OK, 2); }
M(B_Stop) { pthread_mutex_lock(&g_mix); BUF->playing = 0; pthread_mutex_unlock(&g_mix); RET(c, HR_OK, 1); }
M(B_Restore) { RET(c, HR_OK, 1); }
M(B_Initialize) { RET(c, HR_OK, 3); }

/* ---------------------------------------------------------------- 3D buffer / listener */
static float fa(CPU *c, int n) { uint32_t v = ARG(c, n); float f; memcpy(&f, &v, 4); return f; }
M(T_QI) { s32(ARG(c, 2), 0); RET(c, E_NOINTERFACE, 3); }
M(T_Release) { RET(c, com_release(THIS), 1); }
M(T_SetPosition) { SBuf *s = BUF; s->p3[0] = fa(c, 1); s->p3[1] = fa(c, 2); s->p3[2] = fa(c, 3); RET(c, HR_OK, 5); }
M(T_SetVelocity) { RET(c, HR_OK, 5); }
M(T_SetMinDistance) { BUF->mind = fa(c, 1); RET(c, HR_OK, 3); }
M(T_SetMaxDistance) { BUF->maxd = fa(c, 1); RET(c, HR_OK, 3); }
M(T_SetMode) { BUF->mode3d = ARG(c, 1); RET(c, HR_OK, 3); }
M(T_SetConeAngles) { RET(c, HR_OK, 4); }
M(T_SetConeOrientation) { RET(c, HR_OK, 5); }
M(T_SetConeOutsideVolume) { RET(c, HR_OK, 3); }
M(T_SetAllParameters) {
    SBuf *s = BUF; uint32_t p = ARG(c, 1);
    memcpy(s->p3, gptr(p + 4), 12);
    memcpy(&s->mind, gptr(p + 52), 4); memcpy(&s->maxd, gptr(p + 56), 4); s->mode3d = g32(p + 60);
    RET(c, HR_OK, 3);
}
M(T_GetPosition) { memcpy(gptr(ARG(c, 1)), BUF->p3, 12); RET(c, HR_OK, 2); }
M(T_Get4) { RET(c, HR_OK, 2); }
M(L_SetPosition) { g_lis_pos[0] = fa(c, 1); g_lis_pos[1] = fa(c, 2); g_lis_pos[2] = fa(c, 3); RET(c, HR_OK, 5); }
M(L_SetOrientation) {
    for (int k = 0; k < 3; k++) { g_lis_front[k] = fa(c, 1 + k); g_lis_top[k] = fa(c, 4 + k); }
    RET(c, HR_OK, 8);
}
M(L_SetVelocity) { RET(c, HR_OK, 5); }
M(L_SetDistanceFactor) { g_distf = fa(c, 1); RET(c, HR_OK, 3); }
M(L_SetRolloffFactor) { g_rolloff = fa(c, 1); RET(c, HR_OK, 3); }
M(L_SetDopplerFactor) { RET(c, HR_OK, 3); }
M(L_Commit) { RET(c, HR_OK, 1); }
M(L_SetAll) {
    uint32_t p = ARG(c, 1);
    memcpy(g_lis_pos, gptr(p + 4), 12); memcpy(g_lis_front, gptr(p + 28), 12); memcpy(g_lis_top, gptr(p + 40), 12);
    memcpy(&g_distf, gptr(p + 52), 4); memcpy(&g_rolloff, gptr(p + 56), 4);
    RET(c, HR_OK, 3);
}

/* ---------------------------------------------------------------- IDirectSound8 */
M(DS_QI) { s32(ARG(c, 2), 0); RET(c, E_NOINTERFACE, 3); }
M(DS_Release) { RET(c, com_release(THIS), 1); }
M(DS_CreateSoundBuffer) {
    uint32_t d = ARG(c, 1), flags = g32(d + 4), bytes = g32(d + 8), wfx = g32(d + 16);
    SBuf *s = new_buf();
    s->primary = (flags & 1) != 0;
    s->is3d = (flags & 0x10) != 0;
    s->channels = 2; s->bits = 16; s->rate = 22050;
    if (wfx) { s->channels = g16(wfx + 2); s->rate = g32(wfx + 4); s->bits = g16(wfx + 14); }
    if (!s->primary) { s->size = bytes; s->mem = galloc(bytes + 16, 1); }
    s->obj = com_new(vt_buf, s);
    s32(ARG(c, 2), s->obj);
    RET(c, HR_OK, 4);
}
M(DS_GetCaps) { memset(gptr(ARG(c, 1)) + 4, 0, 92); RET(c, HR_OK, 2); }
M(DS_DuplicateSoundBuffer) {
    SBuf *src = com_host(ARG(c, 1));
    SBuf *s = new_buf();
    *s = *src;
    s->obj3d = 0; s->playing = 0; s->pos = 0;
    s->mem = galloc(src->size + 16, 0);
    memcpy(gptr(s->mem), gptr(src->mem), src->size);
    s->obj = com_new(vt_buf, s);
    s32(ARG(c, 2), s->obj);
    RET(c, HR_OK, 3);
}
M(DS_SetCooperativeLevel) { audio_start(); RET(c, HR_OK, 3); }
M(DS_Compact) { RET(c, HR_OK, 1); }
M(DS_GetSpeakerConfig) { s32(ARG(c, 1), 4); RET(c, HR_OK, 2); }
M(DS_SetSpeakerConfig) { RET(c, HR_OK, 2); }
M(DS_Initialize) { RET(c, HR_OK, 2); }

static void build(void) {
    static const char *const ds_n[] = {"QueryInterface", "AddRef", "Release", "CreateSoundBuffer", "GetCaps", "DuplicateSoundBuffer",
        "SetCooperativeLevel", "Compact", "GetSpeakerConfig", "SetSpeakerConfig", "Initialize", "VerifyCertification"};
    static const GuestFn ds_f[] = {DS_QI, AddRef, DS_Release, DS_CreateSoundBuffer, DS_GetCaps, DS_DuplicateSoundBuffer,
        DS_SetCooperativeLevel, DS_Compact, DS_GetSpeakerConfig, DS_SetSpeakerConfig, DS_Initialize, NULL};
    vt_ds = com_vtable("IDirectSound8", ds_n, ds_f, 12);
    static const char *const b_n[] = {"QueryInterface", "AddRef", "Release", "GetCaps", "GetCurrentPosition", "GetFormat", "GetVolume",
        "GetPan", "GetFrequency", "GetStatus", "Initialize", "Lock", "Play", "SetCurrentPosition", "SetFormat", "SetVolume",
        "SetPan", "SetFrequency", "Stop", "Unlock", "Restore", "SetFX", "AcquireResources", "GetObjectInPath"};
    static const GuestFn b_f[] = {B_QueryInterface, AddRef, B_Release, B_GetCaps, B_GetCurrentPosition, B_GetFormat, B_GetVolume,
        B_GetPan, B_GetFrequency, B_GetStatus, B_Initialize, B_Lock, B_Play, B_SetCurrentPosition, B_SetFormat, B_SetVolume,
        B_SetPan, B_SetFrequency, B_Stop, B_Unlock, B_Restore, NULL, NULL, NULL};
    vt_buf = com_vtable("IDirectSoundBuffer8", b_n, b_f, 24);
    static const char *const t_n[] = {"QueryInterface", "AddRef", "Release", "GetAllParameters", "GetConeAngles", "GetConeOrientation",
        "GetConeOutsideVolume", "GetMaxDistance", "GetMinDistance", "GetMode", "GetPosition", "GetVelocity", "SetAllParameters",
        "SetConeAngles", "SetConeOrientation", "SetConeOutsideVolume", "SetMaxDistance", "SetMinDistance", "SetMode",
        "SetPosition", "SetVelocity"};
    static const GuestFn t_f[] = {T_QI, AddRef, T_Release, T_Get4, T_Get4, T_Get4, T_Get4, T_Get4, T_Get4, T_Get4, T_GetPosition,
        T_Get4, T_SetAllParameters, T_SetConeAngles, T_SetConeOrientation, T_SetConeOutsideVolume, T_SetMaxDistance,
        T_SetMinDistance, T_SetMode, T_SetPosition, T_SetVelocity};
    vt_3d = com_vtable("IDirectSound3DBuffer", t_n, t_f, 21);
    static const char *const l_n[] = {"QueryInterface", "AddRef", "Release", "GetAllParameters", "GetDistanceFactor", "GetDopplerFactor",
        "GetOrientation", "GetPosition", "GetRolloffFactor", "GetVelocity", "SetAllParameters", "SetDistanceFactor",
        "SetDopplerFactor", "SetOrientation", "SetPosition", "SetRolloffFactor", "SetVelocity", "CommitDeferredSettings"};
    static const GuestFn l_f[] = {T_QI, AddRef, T_Release, T_Get4, T_Get4, T_Get4, T_Get4, T_Get4, T_Get4, T_Get4, L_SetAll,
        L_SetDistanceFactor, L_SetDopplerFactor, L_SetOrientation, L_SetPosition, L_SetRolloffFactor, L_SetVelocity, L_Commit};
    vt_lis = com_vtable("IDirectSound3DListener", l_n, l_f, 18);
}

void shim_DSOUND_ord11(CPU *c) {   /* DirectSoundCreate8(lpcGuid, ppDS8, pUnkOuter) */
    if (!vt_ds) build();
    if (!g_ds_obj) g_ds_obj = com_new(vt_ds, NULL);
    s32(ARG(c, 1), g_ds_obj);
    audio_start();
    LOG("[dsound] DirectSoundCreate8\n");
    RET(c, HR_OK, 3);
}
