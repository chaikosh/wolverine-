/* D3D8 vertex shader objects. Declarations map to FVF for the fixed-function path;
   vs_1_1 bytecode is stored for the (pending) MSL translator. */
#include "d3d8_int.h"
#include <stdlib.h>

static VShader *g_vs[512];

static uint32_t decl_to_fvf(const uint32_t *d, int n) {
    uint32_t fvf = 0, ntex = 0;
    for (int i = 0; i < n; i++) {
        uint32_t t = d[i], kind = t >> 29;
        if (kind != 2 || (t & 0x10000000u)) continue;          /* only REG tokens (not SKIP) */
        uint32_t reg = t & 0x1F, type = (t >> 16) & 15;
        switch (reg) {
        case 0: fvf |= FVF_XYZ; break;                          /* D3DVSDE_POSITION */
        case 3: fvf |= FVF_NORMAL; break;
        case 5: fvf |= FVF_DIFFUSE; break;
        case 6: fvf |= FVF_SPECULAR; break;
        default:
            if (reg >= 7 && reg <= 14) {
                uint32_t k = reg - 7, code = type == 0 ? 3 : type == 1 ? 0 : type == 2 ? 1 : 2;
                fvf |= code << (16 + 2 * k);
                if (k + 1 > ntex) ntex = k + 1;
            }
        }
    }
    return fvf | (ntex << 8);
}

uint32_t vs_create(Dev *d, const uint32_t *decl, const uint32_t *code) {
    (void)d;
    for (uint32_t i = 1; i < 512; i++) {
        if (g_vs[i]) continue;
        VShader *v = calloc(1, sizeof *v);
        v->handle = (i << 1) | 1;
        if (decl) {
            while (v->ndecl < 63 && decl[v->ndecl] != 0xFFFFFFFFu) { v->decl[v->ndecl] = decl[v->ndecl]; v->ndecl++; }
            v->fvf_equiv = decl_to_fvf(v->decl, v->ndecl);
        }
        if (code) {
            int n = 0;
            while (code[n] != 0x0000FFFFu && n < 65536) n++;
            v->code = malloc(sizeof(uint32_t) * (size_t)(n + 1));
            memcpy(v->code, code, sizeof(uint32_t) * (size_t)(n + 1));
            v->ncode = n;
        }
        LOG("[d3d] CreateVertexShader #%u decl=%d tokens fvf~%08X code=%d tokens\n", i, v->ndecl, v->fvf_equiv, v->ncode);
        g_vs[i] = v;
        return v->handle;
    }
    return 0;
}
VShader *vs_get(uint32_t h) { uint32_t i = h >> 1; return (h & 1) && i < 512 ? g_vs[i] : NULL; }
void vs_delete(uint32_t h) {
    uint32_t i = h >> 1;
    if ((h & 1) && i < 512 && g_vs[i]) { free(g_vs[i]->code); free(g_vs[i]); g_vs[i] = NULL; }
}
