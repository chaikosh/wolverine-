/* WINMM mmio*: RIFF/WAVE parsing over host files or guest memory (FOURCC_MEM). */
#include "win32.h"
#include <stdlib.h>
#include <ctype.h>

#define S(n) void shim_WINMM_##n(CPU *c)
#define FOURCC_MEM 0x204D454Du   /* 'MEM ' */
#define FCC_RIFF 0x46464952u
#define FCC_LIST 0x5453494Cu

typedef struct { int used; uint8_t *data; uint32_t size, pos; int own; } MMF;
static MMF g_mm[64];

static MMF *mm_get(uint32_t h) { uint32_t i = h - 0x4000u; return (h >= 0x4000u && i < 64 && g_mm[i].used) ? &g_mm[i] : NULL; }
static uint32_t rd32(MMF *f, uint32_t at) { uint32_t v = 0; if (at + 4 <= f->size) memcpy(&v, f->data + at, 4); return v; }

S(mmioOpenA) {
    uint32_t name = ARG(c, 0), info = ARG(c, 1);
    int slot = -1;
    for (int i = 0; i < 64; i++) if (!g_mm[i].used) { slot = i; break; }
    if (slot < 0) { RET(c, 0, 3); return; }
    MMF *f = &g_mm[slot];
    memset(f, 0, sizeof *f);
    if (info && g32(info + 4) == FOURCC_MEM) {            /* memory file */
        f->data = gptr(g32(info + 24)); f->size = g32(info + 20);
    } else {
        char hp[1024];
        if (!name || !host_path(gstr(name), hp, sizeof hp, 1)) { if (info) s16(info + 12, 257); RET(c, 0, 3); return; }
        FILE *fh = fopen(hp, "rb");
        if (!fh) { RET(c, 0, 3); return; }
        fseek(fh, 0, SEEK_END); f->size = (uint32_t)ftell(fh); fseek(fh, 0, SEEK_SET);
        f->data = malloc(f->size ? f->size : 1); fread(f->data, 1, f->size, fh); fclose(fh);
        f->own = 1;
    }
    f->used = 1;
    RET(c, 0x4000u + (uint32_t)slot, 3);
}
S(mmioClose) {
    MMF *f = mm_get(ARG(c, 0));
    if (f) { if (f->own) free(f->data); f->used = 0; }
    RET(c, 0, 2);
}
S(mmioRead) {
    MMF *f = mm_get(ARG(c, 0));
    uint32_t dst = ARG(c, 1); int32_t n = (int32_t)ARG(c, 2);
    if (!f || n < 0) { RET(c, 0xFFFFFFFFu, 3); return; }
    if (f->pos + (uint32_t)n > f->size) n = (int32_t)(f->size - f->pos);
    memcpy(gptr(dst), f->data + f->pos, (size_t)n);
    f->pos += (uint32_t)n;
    RET(c, (uint32_t)n, 3);
}
S(mmioDescend) {
    MMF *f = mm_get(ARG(c, 0));
    uint32_t ck = ARG(c, 1), parent = ARG(c, 2), flags = ARG(c, 3);
    if (!f) { RET(c, 265, 4); return; }
    uint32_t end = parent ? g32(parent + 12) + g32(parent + 4) : f->size;
    if (end > f->size) end = f->size;
    uint32_t want_id = g32(ck), want_type = g32(ck + 8);
    for (;;) {
        if (f->pos + 8 > end) { RET(c, 265, 4); return; }              /* MMIOERR_CHUNKNOTFOUND */
        uint32_t id = rd32(f, f->pos), size = rd32(f, f->pos + 4), start = f->pos;
        uint32_t type = (id == FCC_RIFF || id == FCC_LIST) ? rd32(f, f->pos + 8) : 0;
        int match = 1;
        if (flags & 0x20) match = id == FCC_RIFF && type == want_type;        /* MMIO_FINDRIFF */
        else if (flags & 0x40) match = id == FCC_LIST && type == want_type;   /* MMIO_FINDLIST */
        else if (flags & 0x10) match = id == want_id;                         /* MMIO_FINDCHUNK */
        if (match) {
            s32(ck, id); s32(ck + 4, size); s32(ck + 8, type); s32(ck + 12, start + 8); s32(ck + 16, 0);
            f->pos = start + 8 + (type ? 4 : 0);
            RET(c, 0, 4);
            return;
        }
        f->pos = start + 8 + ((size + 1) & ~1u);
    }
}
S(mmioAscend) {
    MMF *f = mm_get(ARG(c, 0));
    uint32_t ck = ARG(c, 1);
    if (f) f->pos = g32(ck + 12) + ((g32(ck + 4) + 1) & ~1u);
    RET(c, 0, 3);
}
S(mmioStringToFOURCCA) {
    const char *s = gstr(ARG(c, 0));
    uint32_t flags = ARG(c, 1), v = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t ch = (s && i < (int)strlen(s)) ? (uint8_t)s[i] : ' ';
        if (flags & 0x10) ch = (uint8_t)toupper(ch);
        v |= (uint32_t)ch << (8 * i);
    }
    RET(c, v, 2);
}
