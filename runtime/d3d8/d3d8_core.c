/* Direct3D 8 COM surface: IDirect3D8, device state, resources. Rendering lives in d3d8_metal.m. */
#include "d3d8_int.h"
#include "../savestate.h"
#include <stdlib.h>
#include <math.h>

Dev *g_dev;
static uint32_t g_d3d_obj, vt_d3d, vt_dev, vt_tex, vt_cube, vt_surf, vt_vb, vt_ib;
static int g_trace_d3d;

#define M(name) static void name(CPU *c)
#define DEV ((Dev *)com_host(THIS))
#define TRACE(...) do { if (g_trace_d3d) LOG("[d3d] " __VA_ARGS__); } while (0)

static float f32(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

/* ===================================================================== IUnknown */
M(QueryInterface) { if (ARG(c, 2)) s32(ARG(c, 2), 0); RET(c, E_NOINTERFACE, 3); }
M(AddRef) { RET(c, com_addref(THIS), 1); }
M(ReleaseNoFree) { RET(c, com_release(THIS), 1); }

/* ===================================================================== IDirect3D8 */
static uint32_t g_modes[12][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 960}, {1366, 768},
                                   {1600, 900}, {1920, 1080}};
static int g_nmodes = 8;
void plat_native_mode(int *w, int *h);
/* Phone-shaped mode: screen aspect at 900 lines (matches what core.c writes into Wolvie.INI on iOS). */
static void add_native_mode(void) {
    static int done;
    if (done) return;
    done = 1;
    int w, h;
    plat_native_mode(&w, &h);
    const char *em = getenv("EXTRA_MODE");   /* e.g. "1952x900" for testing phone-shaped modes on a Mac */
    if (em && sscanf(em, "%dx%d", &w, &h) == 2) {
        g_modes[g_nmodes][0] = (uint32_t)w; g_modes[g_nmodes][1] = (uint32_t)h; g_nmodes++;
        return;
    }
    if (w <= 0 || h <= 0) return;
    uint32_t mh = 900, mw = ((uint32_t)((double)w * mh / h) + 7) & ~7u;
    for (int i = 0; i < g_nmodes; i++) if (g_modes[i][0] == mw && g_modes[i][1] == mh) return;
    g_modes[g_nmodes][0] = mw; g_modes[g_nmodes][1] = mh; g_nmodes++;
}
#define NMODES g_nmodes

M(D3D_GetAdapterCount) { RET(c, 1, 1); }
M(D3D_GetAdapterIdentifier) {
    uint32_t p = ARG(c, 3);
    memset(gptr(p), 0, 1068);
    strcpy((char *)gptr(p), "metal.dll");
    strcpy((char *)gptr(p + 512), "Apple Metal (Wolverine recompiled)");
    s32(p + 1032, 0x106B); s32(p + 1036, 0x0001);
    RET(c, HR_OK, 4);
}
M(D3D_GetAdapterModeCount) { add_native_mode(); RET(c, (uint32_t)NMODES, 2); }
M(D3D_EnumAdapterModes) {
    uint32_t i = ARG(c, 2), p = ARG(c, 3);
    if (i >= NMODES) { RET(c, D3DERR_INVALIDCALL, 4); return; }
    s32(p, g_modes[i][0]); s32(p + 4, g_modes[i][1]); s32(p + 8, 60); s32(p + 12, D3DFMT_X8R8G8B8);
    RET(c, HR_OK, 4);
}
M(D3D_GetAdapterDisplayMode) {
    uint32_t p = ARG(c, 2);
    s32(p, 1366); s32(p + 4, 768); s32(p + 8, 60); s32(p + 12, D3DFMT_X8R8G8B8);
    RET(c, HR_OK, 3);
}
M(D3D_CheckDeviceType) { RET(c, HR_OK, 6); }
static int tex_format_ok(uint32_t f) {
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_A8L8: case D3DFMT_DXT1: case D3DFMT_DXT2:
    case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return 1;
    default: return 0;
    }
}
M(D3D_CheckDeviceFormat) {
    uint32_t usage = ARG(c, 4), f = ARG(c, 6);
    int ok;
    if (usage & D3DUSAGE_DEPTHSTENCIL) ok = f == D3DFMT_D24S8 || f == D3DFMT_D16 || f == D3DFMT_D24X8 || f == D3DFMT_D32;
    else if (usage & D3DUSAGE_RENDERTARGET) ok = f == D3DFMT_A8R8G8B8 || f == D3DFMT_X8R8G8B8 || f == D3DFMT_R5G6B5;
    else ok = tex_format_ok(f);
    RET(c, ok ? HR_OK : D3DERR_NOTAVAILABLE, 7);
}
M(D3D_CheckDeviceMultiSampleType) { RET(c, ARG(c, 5) ? D3DERR_NOTAVAILABLE : HR_OK, 6); }
M(D3D_CheckDepthStencilMatch) { RET(c, HR_OK, 6); }
static void fill_caps(uint32_t p) {
    memset(gptr(p), 0, 212);
    s32(p + 0, 1);                       /* HAL */
    s32(p + 8, 0x00020000);              /* D3DCAPS_READ_SCANLINE */
    s32(p + 12, 0x00020000 | 0x40000000);/* Caps2: FULLSCREENGAMMA, DYNAMICTEXTURES */
    s32(p + 20, 0x80000001);             /* intervals: IMMEDIATE | ONE */
    s32(p + 28, 0x001BFEF0);             /* DevCaps incl. HWTRANSFORMANDLIGHT, DRAWPRIMITIVES2EX, PUREDEVICE */
    s32(p + 32, 0x000000F2 | 0x80);      /* PrimitiveMiscCaps: CULL*, COLORWRITEENABLE, BLENDOP */
    s32(p + 36, 0x00F7D1F1);             /* RasterCaps: ZTEST, FOGVERTEX, FOGTABLE, MIPMAPLODBIAS, ZBIAS, FOGRANGE... */
    s32(p + 40, 0xFF); s32(p + 44, 0x1FFF); s32(p + 48, 0x1FFF); s32(p + 52, 0xFF);
    s32(p + 56, 0x00084208);             /* ShadeCaps */
    s32(p + 60, 0x0000E145);             /* TextureCaps: PERSPECTIVE, ALPHA, MIPMAP, CUBEMAP, PROJECTED... */
    s32(p + 64, 0x03030303); s32(p + 68, 0x03030303); s32(p + 72, 0x03030303);
    s32(p + 76, 0x3F); s32(p + 80, 0x3F); s32(p + 84, 0x1F);
    s32(p + 88, 4096); s32(p + 92, 4096); s32(p + 96, 256); s32(p + 100, 8192); s32(p + 104, 4096); s32(p + 108, 16);
    float mw = 1e10f, gb = 8192.f; memcpy(gptr(p + 112), &mw, 4);
    float g4[4] = {-gb, -gb, gb, gb}; memcpy(gptr(p + 116), g4, 16);
    s32(p + 136, 0xFF);                  /* StencilCaps */
    s32(p + 140, 0x00080008);            /* FVFCaps: 8 texcoords, PSIZE */
    s32(p + 144, 0x03FFFFFF);            /* TextureOpCaps */
    s32(p + 148, 8); s32(p + 152, MAX_STAGES);
    s32(p + 156, 0x3B);                  /* VertexProcessingCaps */
    s32(p + 160, 8); s32(p + 164, 6); s32(p + 168, 4); s32(p + 172, 0);
    float ps = 64.f; memcpy(gptr(p + 176), &ps, 4);
    s32(p + 180, 0xFFFFF); s32(p + 184, 0xFFFFF); s32(p + 188, 16); s32(p + 192, 256);
    s32(p + 196, 0xFFFE0101);            /* vs_1_1 */
    s32(p + 200, 96);
    s32(p + 204, 0);                     /* no pixel shaders: keep the fixed-function combiner path */
}
M(D3D_GetDeviceCaps) { fill_caps(ARG(c, 3)); RET(c, HR_OK, 4); }
M(D3D_GetAdapterMonitor) { RET(c, 1, 2); }

/* Every resource ever created, in creation order. Host structs are never freed, so a save state can
   find the same objects again and only has to restore their fields (and Metal textures). */
typedef struct { void **p; int n, cap; } Reg;
static Reg g_rtex, g_rsurf, g_rbuf;
static void *reg(Reg *r, void *p) {
    if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 1024; r->p = realloc(r->p, sizeof(void *) * (size_t)r->cap); }
    r->p[r->n++] = p;
    return p;
}

static uint32_t new_surface(int kind, uint32_t w, uint32_t h, uint32_t fmt) {
    Surf *s = reg(&g_rsurf, calloc(1, sizeof(Surf)));
    s->kind = kind; s->w = w; s->h = h; s->fmt = fmt;
    s->obj = com_new(vt_surf, s);
    return s->obj;
}

static void dev_defaults(Dev *d) {
    static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    memcpy(d->view.m, ident, 64); memcpy(d->proj.m, ident, 64);
    for (int i = 0; i < 4; i++) memcpy(d->world[i].m, ident, 64);
    for (int i = 0; i < 8; i++) memcpy(d->texm[i].m, ident, 64);
    uint32_t *rs = d->rs;
    rs[RS_ZENABLE] = 1; rs[RS_FILLMODE] = 3; rs[RS_SHADEMODE] = 2; rs[RS_ZWRITEENABLE] = 1; rs[RS_SRCBLEND] = 2;
    rs[RS_DESTBLEND] = 1; rs[RS_CULLMODE] = 3; rs[RS_ZFUNC] = 4; rs[RS_ALPHAFUNC] = 8; rs[RS_FOGTABLEMODE] = 0;
    float one = 1.0f; memcpy(&rs[RS_FOGEND], &one, 4); memcpy(&rs[RS_FOGDENSITY], &one, 4);
    rs[RS_STENCILFAIL] = rs[RS_STENCILZFAIL] = rs[RS_STENCILPASS] = 1; rs[RS_STENCILFUNC] = 8;
    rs[RS_STENCILMASK] = rs[RS_STENCILWRITEMASK] = 0xFFFFFFFFu; rs[RS_TEXTUREFACTOR] = 0xFFFFFFFFu;
    rs[RS_CLIPPING] = 1; rs[RS_LIGHTING] = 1; rs[RS_COLORVERTEX] = 1; rs[RS_LOCALVIEWER] = 1;
    rs[RS_DIFFUSEMATERIALSOURCE] = 1; rs[RS_SPECULARMATERIALSOURCE] = 2; rs[RS_COLORWRITEENABLE] = 0xF;
    rs[RS_BLENDOP] = 1; memcpy(&rs[RS_POINTSIZE], &one, 4);
    for (int s = 0; s < 8; s++) {
        uint32_t *t = d->tss[s];
        t[TSS_COLOROP] = s == 0 ? 4 : 1; t[TSS_COLORARG1] = 2; t[TSS_COLORARG2] = 1;
        t[TSS_ALPHAOP] = s == 0 ? 2 : 1; t[TSS_ALPHAARG1] = 2; t[TSS_ALPHAARG2] = 1;
        t[TSS_TEXCOORDINDEX] = (uint32_t)s; t[TSS_ADDRESSU] = t[TSS_ADDRESSV] = t[TSS_ADDRESSW] = 1;
        t[TSS_MAGFILTER] = t[TSS_MINFILTER] = 1; t[TSS_MIPFILTER] = 0; t[TSS_MAXANISOTROPY] = 1;
        t[TSS_RESULTARG] = 1;
    }
    d->vp.x = 0; d->vp.y = 0; d->vp.w = d->bbw; d->vp.h = d->bbh; d->vp.minz = 0; d->vp.maxz = 1;
    for (int i = 0; i < 17; i++) d->material[i] = 0;
    d->material[0] = d->material[1] = d->material[2] = d->material[3] = 1;   /* diffuse */
}

M(D3D_CreateDevice) {
    uint32_t pp = ARG(c, 5), out = ARG(c, 6);
    Dev *d = calloc(1, sizeof *d);
    d->bbw = g32(pp); d->bbh = g32(pp + 4); d->bbfmt = g32(pp + 8);
    if (!d->bbw) d->bbw = 1366;
    if (!d->bbh) d->bbh = 768;
    d->depthfmt = g32(pp + 32) ? g32(pp + 36) : 0;
    LOG("[d3d] CreateDevice %ux%u fmt=%u windowed=%u autodepth=%u(%u) flags=%08X\n", d->bbw, d->bbh, d->bbfmt,
        g32(pp + 28), g32(pp + 32), g32(pp + 36), ARG(c, 4));
    g_dev = d;
    dev_defaults(d);
    d->obj = com_new(vt_dev, d);
    mtl_init(d);
    d->backbuf_surf = new_surface(1, d->bbw, d->bbh, D3DFMT_X8R8G8B8);
    d->rt = com_host(d->backbuf_surf);
    if (d->depthfmt) { d->depth_surf = new_surface(2, d->bbw, d->bbh, d->depthfmt); d->ds = com_host(d->depth_surf); }
    mtl_set_rendertarget(d);
    s32(out, d->obj);
    RET(c, HR_OK, 7);
}

/* ===================================================================== state blocks */
typedef struct SBEntry { uint8_t kind; uint32_t a, b; uint32_t v[26]; } SBEntry;
typedef struct StateBlock { SBEntry *e; int n, cap; } StateBlock;
static StateBlock *g_sb[1024];
static int g_nsb = 1;
static StateBlock *g_recording;
enum { SB_RS, SB_TSS, SB_TEX, SB_XF, SB_VS, SB_MAT, SB_LIGHT, SB_LIGHTEN, SB_VP, SB_STREAM, SB_IDX, SB_VSC, SB_PS };

static void sb_add(StateBlock *b, uint8_t kind, uint32_t a, uint32_t bb, const void *v, size_t n) {
    for (int i = 0; i < b->n; i++)   /* overwrite existing entry for the same slot */
        if (b->e[i].kind == kind && b->e[i].a == a && b->e[i].b == bb) { memcpy(b->e[i].v, v, n); return; }
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 32; b->e = realloc(b->e, sizeof(SBEntry) * (size_t)b->cap); }
    SBEntry *e = &b->e[b->n++];
    e->kind = kind; e->a = a; e->b = bb;
    memcpy(e->v, v, n);
}
static Mat *xf_slot(Dev *d, uint32_t t) {
    if (t == 2) return &d->view;
    if (t == 3) return &d->proj;
    if (t >= 16 && t < 24) return &d->texm[t - 16];
    if (t >= 256 && t < 260) return &d->world[t - 256];
    return NULL;
}
static void set_texture(Dev *d, uint32_t stage, uint32_t obj);
static void sb_apply(Dev *d, StateBlock *b) {
    for (int i = 0; i < b->n; i++) {
        SBEntry *e = &b->e[i];
        switch (e->kind) {
        case SB_RS: d->rs[e->a & 255] = e->v[0]; break;
        case SB_TSS: d->tss[e->a & 7][e->b & 31] = e->v[0]; break;
        case SB_TEX: set_texture(d, e->a, e->v[0]); break;
        case SB_XF: { Mat *m = xf_slot(d, e->a); if (m) memcpy(m->m, e->v, 64); break; }
        case SB_VS: d->vs = e->v[0]; break;
        case SB_MAT: memcpy(d->material, e->v, 68); break;
        case SB_LIGHT: memcpy(&d->lights[e->a & 7], e->v, sizeof(Light)); break;
        case SB_LIGHTEN: d->light_on[e->a & 7] = (int)e->v[0]; break;
        case SB_VP: memcpy(&d->vp, e->v, sizeof d->vp); break;
        case SB_STREAM: d->stream[e->a & 15].vb_obj = e->v[0]; d->stream[e->a & 15].vb = com_host(e->v[0]); d->stream[e->a & 15].stride = e->v[1]; break;
        case SB_IDX: d->ib_obj = e->v[0]; d->ib = com_host(e->v[0]); d->base_vertex = e->v[1]; break;
        case SB_VSC: memcpy(d->vsc[e->a % 96], e->v, 16); break;
        case SB_PS: d->ps = e->v[0]; break;
        }
    }
}
static void sb_capture(Dev *d, StateBlock *b) {
    for (int i = 0; i < b->n; i++) {
        SBEntry *e = &b->e[i];
        switch (e->kind) {
        case SB_RS: e->v[0] = d->rs[e->a & 255]; break;
        case SB_TSS: e->v[0] = d->tss[e->a & 7][e->b & 31]; break;
        case SB_TEX: e->v[0] = d->tex_obj[e->a & 7]; break;
        case SB_XF: { Mat *m = xf_slot(d, e->a); if (m) memcpy(e->v, m->m, 64); break; }
        case SB_VS: e->v[0] = d->vs; break;
        case SB_MAT: memcpy(e->v, d->material, 68); break;
        case SB_LIGHT: memcpy(e->v, &d->lights[e->a & 7], sizeof(Light)); break;
        case SB_LIGHTEN: e->v[0] = (uint32_t)d->light_on[e->a & 7]; break;
        case SB_VP: memcpy(e->v, &d->vp, sizeof d->vp); break;
        case SB_STREAM: e->v[0] = d->stream[e->a & 15].vb_obj; e->v[1] = d->stream[e->a & 15].stride; break;
        case SB_IDX: e->v[0] = d->ib_obj; e->v[1] = d->base_vertex; break;
        case SB_VSC: memcpy(e->v, d->vsc[e->a % 96], 16); break;
        case SB_PS: e->v[0] = d->ps; break;
        }
    }
}
#define RECORD(kind, a, b, v, n) if (g_recording) { sb_add(g_recording, kind, a, b, v, n); RET(c, HR_OK, NARGS); return; }

/* ===================================================================== device */
M(Dev_TestCooperativeLevel) { RET(c, HR_OK, 1); }
M(Dev_GetAvailableTextureMem) { RET(c, 512u << 20, 1); }
M(Dev_ResourceManagerDiscardBytes) { RET(c, HR_OK, 2); }
M(Dev_GetDirect3D) { s32(ARG(c, 1), g_d3d_obj); com_addref(g_d3d_obj); RET(c, HR_OK, 2); }
M(Dev_GetDeviceCaps) { fill_caps(ARG(c, 1)); RET(c, HR_OK, 2); }
M(Dev_GetDisplayMode) {
    uint32_t p = ARG(c, 1);
    s32(p, DEV->bbw); s32(p + 4, DEV->bbh); s32(p + 8, 60); s32(p + 12, D3DFMT_X8R8G8B8);
    RET(c, HR_OK, 2);
}
M(Dev_GetCreationParameters) { uint32_t p = ARG(c, 1); s32(p, 0); s32(p + 4, 1); s32(p + 8, 0x00010010u); s32(p + 12, 0x40); RET(c, HR_OK, 2); }
M(Dev_SetCursorProperties) { RET(c, HR_OK, 4); }
M(Dev_SetCursorPosition) { RET(c, 0, 4); }
M(Dev_ShowCursor) { RET(c, 0, 2); }
M(Dev_Reset) { LOG("[d3d] Reset\n"); RET(c, HR_OK, 2); }
M(Dev_Present) {
    mtl_present(DEV);
    ss_present(c);   /* quick save / quick load happen between frames */
    RET(c, HR_OK, 5);
}
M(Dev_GetBackBuffer) { s32(ARG(c, 3), DEV->backbuf_surf); com_addref(DEV->backbuf_surf); RET(c, HR_OK, 4); }
M(Dev_GetRasterStatus) { s32(ARG(c, 1), 1); s32(ARG(c, 1) + 4, 0); RET(c, HR_OK, 2); }
M(Dev_SetGammaRamp) { memcpy(DEV->gamma, gptr(ARG(c, 2)), 3 * 256 * 2); RET(c, 0, 3); }
M(Dev_GetGammaRamp) { memcpy(gptr(ARG(c, 1)), DEV->gamma, 3 * 256 * 2); RET(c, 0, 2); }

extern char g_cur_load[256];
static uint32_t create_texture(Dev *d, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool, int cube) {
    Tex *t = reg(&g_rtex, calloc(1, sizeof(Tex)));
    uint32_t maxl = 1, mw = w, mh = h;
    while (mw > 1 || mh > 1) { mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1; maxl++; }
    if (!levels || levels > maxl) levels = maxl;
    if (fmt_is_dxt(fmt)) {   /* DXT levels below 4x4 still decode via padded blocks */ }
    t->cube = cube; t->w = w; t->h = h; t->levels = levels; t->fmt = fmt; t->usage = usage; t->pool = pool;
    t->mtl = mtl_new_texture(d, w, h, levels, fmt, (usage & D3DUSAGE_RENDERTARGET) != 0, (usage & D3DUSAGE_DEPTHSTENCIL) != 0, cube);
    t->obj = com_new(cube ? vt_cube : vt_tex, t);
    t->hud = strcasestr(g_cur_load, "logan_hud") != NULL;
    { const char *b = strrchr(g_cur_load, '\\'); snprintf(t->src, sizeof t->src, "%s", b ? b + 1 : g_cur_load); }
    TRACE("CreateTexture %ux%u lv=%u fmt=%08X usage=%X -> %08X\n", w, h, levels, fmt, usage, t->obj);
    return t->obj;
}
M(Dev_CreateTexture) {
    s32(ARG(c, 7), create_texture(DEV, ARG(c, 1), ARG(c, 2), ARG(c, 3), ARG(c, 4), ARG(c, 5), ARG(c, 6), 0));
    RET(c, HR_OK, 8);
}
M(Dev_CreateCubeTexture) {
    s32(ARG(c, 6), create_texture(DEV, ARG(c, 1), ARG(c, 1), ARG(c, 2), ARG(c, 3), ARG(c, 4), ARG(c, 5), 1));
    RET(c, HR_OK, 7);
}
static uint32_t create_buffer(uint32_t vt, uint32_t len, uint32_t usage, uint32_t fmt_or_fvf, uint32_t pool, int index) {
    Buf *b = reg(&g_rbuf, calloc(1, sizeof(Buf)));
    b->size = len; b->usage = usage; b->pool = pool;
    if (index) b->fmt = fmt_or_fvf; else b->fvf = fmt_or_fvf;
    b->mem = galloc(len + 16, 1);
    b->obj = com_new(vt, b);
    return b->obj;
}
M(Dev_CreateVertexBuffer) { s32(ARG(c, 5), create_buffer(vt_vb, ARG(c, 1), ARG(c, 2), ARG(c, 3), ARG(c, 4), 0)); RET(c, HR_OK, 6); }
M(Dev_CreateIndexBuffer) { s32(ARG(c, 5), create_buffer(vt_ib, ARG(c, 1), ARG(c, 2), ARG(c, 3), ARG(c, 4), 1)); RET(c, HR_OK, 6); }
M(Dev_CreateRenderTarget) {
    uint32_t o = new_surface(4, ARG(c, 1), ARG(c, 2), ARG(c, 3));
    Surf *s = com_host(o);
    s->mtl = mtl_new_texture(DEV, s->w, s->h, 1, s->fmt, 1, 0, 0);
    s32(ARG(c, 6), o);
    RET(c, HR_OK, 7);
}
M(Dev_CreateDepthStencilSurface) {
    uint32_t o = new_surface(2, ARG(c, 1), ARG(c, 2), ARG(c, 3));
    Surf *s = com_host(o);
    s->mtl = mtl_new_texture(DEV, s->w, s->h, 1, s->fmt, 0, 1, 0);
    s32(ARG(c, 5), o);
    RET(c, HR_OK, 6);
}
M(Dev_CreateImageSurface) {
    uint32_t o = new_surface(3, ARG(c, 1), ARG(c, 2), ARG(c, 3));
    Surf *s = com_host(o);
    s->mem = galloc(fmt_size(s->fmt, s->w, s->h), 1);
    s32(ARG(c, 4), o);
    RET(c, HR_OK, 5);
}
M(Dev_CopyRects) {
    Surf *src = com_host(ARG(c, 1)), *dst = com_host(ARG(c, 4));
    uint32_t rects = ARG(c, 2), n = ARG(c, 3), pts = ARG(c, 5);
    if (!rects || !n) mtl_copy_surface(DEV, src, dst, 0, 0, (int)src->w, (int)src->h, 0, 0);
    else for (uint32_t i = 0; i < n; i++) {
        int l = (int)g32(rects + 16 * i), t = (int)g32(rects + 16 * i + 4), r = (int)g32(rects + 16 * i + 8), b = (int)g32(rects + 16 * i + 12);
        int dx = pts ? (int)g32(pts + 8 * i) : l, dy = pts ? (int)g32(pts + 8 * i + 4) : t;
        mtl_copy_surface(DEV, src, dst, l, t, r - l, b - t, dx, dy);
    }
    RET(c, HR_OK, 6);
}
M(Dev_UpdateTexture) {
    Tex *s = com_host(ARG(c, 1)), *d = com_host(ARG(c, 2));
    for (uint32_t l = 0; l < s->levels && l < d->levels; l++) {
        if (!s->lock[0][l]) continue;
        uint32_t lw = s->w >> l ? s->w >> l : 1, lh = s->h >> l ? s->h >> l : 1, n = fmt_size(s->fmt, lw, lh);
        if (!d->lock[0][l]) d->lock[0][l] = galloc(n, 0);
        memcpy(gptr(d->lock[0][l]), gptr(s->lock[0][l]), n);
        mtl_upload(DEV, d, 0, (int)l);
    }
    RET(c, HR_OK, 3);
}
M(Dev_GetFrontBuffer) { RET(c, HR_OK, 2); }
M(Dev_SetRenderTarget) {
    Dev *d = DEV;
    uint32_t rt = ARG(c, 1), ds = ARG(c, 2);
    if (rt) d->rt = com_host(rt);
    d->ds = ds ? com_host(ds) : NULL;
    TRACE("SetRenderTarget rt=%08X ds=%08X\n", rt, ds);
    mtl_set_rendertarget(d);
    RET(c, HR_OK, 3);
}
M(Dev_GetRenderTarget) { s32(ARG(c, 1), DEV->rt->obj); com_addref(DEV->rt->obj); RET(c, HR_OK, 2); }
M(Dev_GetDepthStencilSurface) {
    Dev *d = DEV;
    s32(ARG(c, 1), d->ds ? d->ds->obj : 0);
    if (d->ds) com_addref(d->ds->obj);
    RET(c, d->ds ? HR_OK : 0x8876086Fu, 2);
}
M(Dev_BeginScene) { DEV->in_scene = 1; RET(c, HR_OK, 1); }
M(Dev_EndScene) { DEV->in_scene = 0; RET(c, HR_OK, 1); }
M(Dev_Clear) {
    float z = f32(ARG(c, 5));
    mtl_clear(DEV, ARG(c, 3), ARG(c, 4), z, ARG(c, 6));
    RET(c, HR_OK, 7);
}
#define NARGS 3
M(Dev_SetTransform) {
    uint32_t t = ARG(c, 1);
    RECORD(SB_XF, t, 0, gptr(ARG(c, 2)), 64);
    Mat *m = xf_slot(DEV, t);
    if (m) memcpy(m->m, gptr(ARG(c, 2)), 64);
    RET(c, HR_OK, 3);
}
#undef NARGS
M(Dev_GetTransform) {
    Mat *m = xf_slot(DEV, ARG(c, 1));
    if (m) memcpy(gptr(ARG(c, 2)), m->m, 64);
    RET(c, HR_OK, 3);
}
static void matmul(float *r, const float *a, const float *b) {
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(r, t, 64);
}
M(Dev_MultiplyTransform) {
    Mat *m = xf_slot(DEV, ARG(c, 1));
    if (m) { float in[16]; memcpy(in, gptr(ARG(c, 2)), 64); matmul(m->m, in, m->m); }
    RET(c, HR_OK, 3);
}
#define NARGS 2
M(Dev_SetViewport) {
    RECORD(SB_VP, 0, 0, gptr(ARG(c, 1)), 24);
    memcpy(&DEV->vp, gptr(ARG(c, 1)), 24);
    RET(c, HR_OK, 2);
}
M(Dev_GetViewport) { memcpy(gptr(ARG(c, 1)), &DEV->vp, 24); RET(c, HR_OK, 2); }
M(Dev_SetMaterial) {
    RECORD(SB_MAT, 0, 0, gptr(ARG(c, 1)), 68);
    memcpy(DEV->material, gptr(ARG(c, 1)), 68);
    RET(c, HR_OK, 2);
}
M(Dev_GetMaterial) { memcpy(gptr(ARG(c, 1)), DEV->material, 68); RET(c, HR_OK, 2); }
#undef NARGS
#define NARGS 3
M(Dev_SetLight) {
    uint32_t i = ARG(c, 1);
    if (i >= 8) { RET(c, HR_OK, 3); return; }
    RECORD(SB_LIGHT, i, 0, gptr(ARG(c, 2)), sizeof(Light));
    memcpy(&DEV->lights[i], gptr(ARG(c, 2)), sizeof(Light));
    RET(c, HR_OK, 3);
}
M(Dev_GetLight) { uint32_t i = ARG(c, 1); if (i < 8) memcpy(gptr(ARG(c, 2)), &DEV->lights[i], sizeof(Light)); RET(c, HR_OK, 3); }
M(Dev_LightEnable) {
    uint32_t i = ARG(c, 1), on = ARG(c, 2) != 0;
    if (i >= 8) { RET(c, HR_OK, 3); return; }
    RECORD(SB_LIGHTEN, i, 0, &on, 4);
    DEV->light_on[i] = (int)on;
    RET(c, HR_OK, 3);
}
M(Dev_GetLightEnable) { uint32_t i = ARG(c, 1); s32(ARG(c, 2), i < 8 ? (uint32_t)DEV->light_on[i] : 0); RET(c, HR_OK, 3); }
M(Dev_SetClipPlane) { RET(c, HR_OK, 3); }
M(Dev_GetClipPlane) { memset(gptr(ARG(c, 2)), 0, 16); RET(c, HR_OK, 3); }
M(Dev_SetRenderState) {
    uint32_t s = ARG(c, 1), v = ARG(c, 2);
    RECORD(SB_RS, s, 0, &v, 4);
    if (s < 256) DEV->rs[s] = v;
    RET(c, HR_OK, 3);
}
M(Dev_GetRenderState) { uint32_t s = ARG(c, 1); s32(ARG(c, 2), s < 256 ? DEV->rs[s] : 0); RET(c, HR_OK, 3); }
#undef NARGS
M(Dev_BeginStateBlock) { g_recording = calloc(1, sizeof(StateBlock)); RET(c, HR_OK, 1); }
M(Dev_EndStateBlock) {
    uint32_t tok = (uint32_t)g_nsb;
    g_sb[g_nsb++ & 1023] = g_recording;
    g_recording = NULL;
    s32(ARG(c, 1), tok);
    RET(c, HR_OK, 2);
}
M(Dev_ApplyStateBlock) { uint32_t t = ARG(c, 1); if (t && t < 1024 && g_sb[t]) sb_apply(DEV, g_sb[t]); RET(c, HR_OK, 2); }
M(Dev_CaptureStateBlock) { uint32_t t = ARG(c, 1); if (t && t < 1024 && g_sb[t]) sb_capture(DEV, g_sb[t]); RET(c, HR_OK, 2); }
M(Dev_DeleteStateBlock) {
    uint32_t t = ARG(c, 1);
    if (t && t < 1024 && g_sb[t]) { free(g_sb[t]->e); free(g_sb[t]); g_sb[t] = NULL; }
    RET(c, HR_OK, 2);
}
M(Dev_CreateStateBlock) {
    Dev *d = DEV;
    uint32_t type = ARG(c, 1);           /* 1 = ALL, 2 = PIXELSTATE, 3 = VERTEXSTATE */
    StateBlock *b = calloc(1, sizeof *b);
    uint32_t z[26] = {0};
    for (uint32_t s = 0; s < 256; s++) sb_add(b, SB_RS, s, 0, z, 4);
    if (type != 3) {
        for (uint32_t st = 0; st < 8; st++) for (uint32_t k = 1; k < 29; k++) sb_add(b, SB_TSS, st, k, z, 4);
    }
    if (type == 1) {
        for (uint32_t st = 0; st < 8; st++) sb_add(b, SB_TEX, st, 0, z, 4);
        sb_add(b, SB_XF, 2, 0, z, 64); sb_add(b, SB_XF, 3, 0, z, 64); sb_add(b, SB_XF, 256, 0, z, 64);
        sb_add(b, SB_MAT, 0, 0, z, 68); sb_add(b, SB_VP, 0, 0, z, 24);
        sb_add(b, SB_STREAM, 0, 0, z, 8); sb_add(b, SB_IDX, 0, 0, z, 8); sb_add(b, SB_PS, 0, 0, z, 4);
    }
    if (type != 2) {
        sb_add(b, SB_VS, 0, 0, z, 4);
        for (uint32_t i = 0; i < 8; i++) { sb_add(b, SB_LIGHT, i, 0, z, sizeof(Light)); sb_add(b, SB_LIGHTEN, i, 0, z, 4); }
    }
    sb_capture(d, b);
    uint32_t tok = (uint32_t)g_nsb;
    g_sb[g_nsb++ & 1023] = b;
    s32(ARG(c, 2), tok);
    RET(c, HR_OK, 3);
}
M(Dev_SetClipStatus) { RET(c, HR_OK, 2); }
M(Dev_GetClipStatus) { memset(gptr(ARG(c, 1)), 0, 8); RET(c, HR_OK, 2); }
M(Dev_GetTexture) {
    uint32_t o = ARG(c, 1) < 8 ? DEV->tex_obj[ARG(c, 1)] : 0;
    s32(ARG(c, 2), o);
    if (o) com_addref(o);
    RET(c, HR_OK, 3);
}
static void set_texture(Dev *d, uint32_t stage, uint32_t obj) {
    if (stage >= 8) return;
    d->tex_obj[stage] = obj;
    d->tex[stage] = obj ? com_host(obj) : NULL;
}
#define NARGS 3
M(Dev_SetTexture) {
    uint32_t st = ARG(c, 1), o = ARG(c, 2);
    RECORD(SB_TEX, st, 0, &o, 4);
    set_texture(DEV, st, o);
    RET(c, HR_OK, 3);
}
#undef NARGS
M(Dev_GetTextureStageState) { uint32_t s = ARG(c, 1), t = ARG(c, 2); s32(ARG(c, 3), (s < 8 && t < 32) ? DEV->tss[s][t] : 0); RET(c, HR_OK, 4); }
#define NARGS 4
M(Dev_SetTextureStageState) {
    uint32_t s = ARG(c, 1), t = ARG(c, 2), v = ARG(c, 3);
    RECORD(SB_TSS, s, t, &v, 4);
    if (s < 8 && t < 32) DEV->tss[s][t] = v;
    RET(c, HR_OK, 4);
}
#undef NARGS
M(Dev_ValidateDevice) { s32(ARG(c, 1), 1); RET(c, HR_OK, 2); }
M(Dev_GetInfo) { RET(c, 1, 4); }
M(Dev_SetPaletteEntries) { RET(c, HR_OK, 3); }
M(Dev_GetPaletteEntries) { RET(c, HR_OK, 3); }
M(Dev_SetCurrentTexturePalette) { RET(c, HR_OK, 2); }
M(Dev_GetCurrentTexturePalette) { s32(ARG(c, 1), 0); RET(c, HR_OK, 2); }

static uint32_t prim_verts(uint32_t type, uint32_t n) {
    switch (type) { case 1: return n; case 2: return n * 2; case 3: return n + 1; case 4: return n * 3; default: return n + 2; }
}
M(Dev_DrawPrimitive) {
    Dev *d = DEV;
    uint32_t type = ARG(c, 1), start = ARG(c, 2), n = ARG(c, 3);
    Buf *vb = d->stream[0].vb;
    uint32_t stride = d->stream[0].stride;
    if (vb && n) mtl_draw(d, type, prim_verts(type, n), gptr(vb->mem + start * stride), stride, NULL, 0, 0, 0);
    RET(c, HR_OK, 4);
}
M(Dev_DrawIndexedPrimitive) {
    Dev *d = DEV;
    uint32_t type = ARG(c, 1), minidx = ARG(c, 2), nv = ARG(c, 3), sidx = ARG(c, 4), n = ARG(c, 5);
    Buf *vb = d->stream[0].vb, *ib = d->ib;
    uint32_t stride = d->stream[0].stride;
    if (vb && ib && n) {
        int i32 = ib->fmt == D3DFMT_INDEX32;
        mtl_draw(d, type, nv, gptr(vb->mem + (d->base_vertex + minidx) * stride), stride,
                 gptr(ib->mem + sidx * (i32 ? 4u : 2u)), prim_verts(type, n), i32, -(int32_t)minidx);
    }
    RET(c, HR_OK, 6);
}
M(Dev_DrawPrimitiveUP) {
    uint32_t type = ARG(c, 1), n = ARG(c, 2), data = ARG(c, 3), stride = ARG(c, 4);
    if (n) mtl_draw(DEV, type, prim_verts(type, n), gptr(data), stride, NULL, 0, 0, 0);
    DEV->stream[0].vb = NULL; DEV->stream[0].vb_obj = 0;   /* D3D8 resets stream 0 after *UP draws */
    RET(c, HR_OK, 5);
}
M(Dev_DrawIndexedPrimitiveUP) {
    uint32_t type = ARG(c, 1), minidx = ARG(c, 2), nv = ARG(c, 3), n = ARG(c, 4), idx = ARG(c, 5), ifmt = ARG(c, 6);
    uint32_t data = ARG(c, 7), stride = ARG(c, 8);
    int i32 = ifmt == D3DFMT_INDEX32;
    if (n) mtl_draw(DEV, type, nv, gptr(data + minidx * stride), stride, gptr(idx), prim_verts(type, n), i32, -(int32_t)minidx);
    DEV->stream[0].vb = NULL; DEV->stream[0].vb_obj = 0; DEV->ib = NULL; DEV->ib_obj = 0;
    RET(c, HR_OK, 9);
}
M(Dev_ProcessVertices) { RET(c, HR_OK, 6); }
M(Dev_CreateVertexShader) {
    uint32_t decl = ARG(c, 1), fn = ARG(c, 2);
    uint32_t h = vs_create(DEV, decl ? (const uint32_t *)gptr(decl) : NULL, fn ? (const uint32_t *)gptr(fn) : NULL);
    s32(ARG(c, 3), h);
    RET(c, HR_OK, 5);
}
#define NARGS 2
M(Dev_SetVertexShader) {
    uint32_t h = ARG(c, 1);
    RECORD(SB_VS, 0, 0, &h, 4);
    DEV->vs = h;
    RET(c, HR_OK, 2);
}
#undef NARGS
M(Dev_GetVertexShader) { s32(ARG(c, 1), DEV->vs); RET(c, HR_OK, 2); }
M(Dev_DeleteVertexShader) { vs_delete(ARG(c, 1)); RET(c, HR_OK, 2); }
M(Dev_SetVertexShaderConstant) {
    uint32_t r = ARG(c, 1), p = ARG(c, 2), n = ARG(c, 3);
    for (uint32_t i = 0; i < n && r + i < 96; i++) {
        if (g_recording) sb_add(g_recording, SB_VSC, r + i, 0, gptr(p + 16 * i), 16);
        else memcpy(DEV->vsc[r + i], gptr(p + 16 * i), 16);
    }
    RET(c, HR_OK, 4);
}
M(Dev_GetVertexShaderConstant) {
    uint32_t r = ARG(c, 1), p = ARG(c, 2), n = ARG(c, 3);
    for (uint32_t i = 0; i < n && r + i < 96; i++) memcpy(gptr(p + 16 * i), DEV->vsc[r + i], 16);
    RET(c, HR_OK, 4);
}
M(Dev_GetVertexShaderDeclaration) { RET(c, D3DERR_INVALIDCALL, 4); }
M(Dev_GetVertexShaderFunction) { RET(c, D3DERR_INVALIDCALL, 4); }
#define NARGS 4
M(Dev_SetStreamSource) {
    uint32_t n = ARG(c, 1), vb = ARG(c, 2), stride = ARG(c, 3);
    uint32_t v[2] = {vb, stride};
    RECORD(SB_STREAM, n, 0, v, 8);
    if (n < 16) { DEV->stream[n].vb_obj = vb; DEV->stream[n].vb = vb ? com_host(vb) : NULL; DEV->stream[n].stride = stride; }
    RET(c, HR_OK, 4);
}
#undef NARGS
M(Dev_GetStreamSource) {
    uint32_t n = ARG(c, 1);
    uint32_t o = n < 16 ? DEV->stream[n].vb_obj : 0;
    s32(ARG(c, 2), o); s32(ARG(c, 3), n < 16 ? DEV->stream[n].stride : 0);
    if (o) com_addref(o);
    RET(c, HR_OK, 4);
}
#define NARGS 3
M(Dev_SetIndices) {
    uint32_t ib = ARG(c, 1), base = ARG(c, 2);
    uint32_t v[2] = {ib, base};
    RECORD(SB_IDX, 0, 0, v, 8);
    DEV->ib_obj = ib; DEV->ib = ib ? com_host(ib) : NULL; DEV->base_vertex = base;
    RET(c, HR_OK, 3);
}
#undef NARGS
M(Dev_GetIndices) { s32(ARG(c, 1), DEV->ib_obj); s32(ARG(c, 2), DEV->base_vertex); if (DEV->ib_obj) com_addref(DEV->ib_obj); RET(c, HR_OK, 3); }
M(Dev_CreatePixelShader) { LOG("[d3d] CreatePixelShader (unsupported, ignoring)\n"); s32(ARG(c, 2), 0x0F000001u); RET(c, HR_OK, 3); }
M(Dev_SetPixelShader) { DEV->ps = ARG(c, 1); RET(c, HR_OK, 2); }
M(Dev_GetPixelShader) { s32(ARG(c, 1), DEV->ps); RET(c, HR_OK, 2); }
M(Dev_DeletePixelShader) { RET(c, HR_OK, 2); }
M(Dev_SetPixelShaderConstant) { RET(c, HR_OK, 4); }
M(Dev_GetPixelShaderConstant) { RET(c, HR_OK, 4); }

/* ===================================================================== resources (shared) */
M(Res_GetDevice) { s32(ARG(c, 1), g_dev->obj); com_addref(g_dev->obj); RET(c, HR_OK, 2); }
M(Res_SetPrivateData) { RET(c, HR_OK, 5); }
M(Res_GetPrivateData) { RET(c, 0x88760866u, 4); }
M(Res_FreePrivateData) { RET(c, HR_OK, 2); }
M(Res_SetPriority) { RET(c, 0, 2); }
M(Res_GetPriority) { RET(c, 0, 1); }
M(Res_PreLoad) { RET(c, 0, 1); }

/* ---- textures ---- */
static uint32_t lvl_dim(uint32_t v, uint32_t l) { v >>= l; return v ? v : 1; }
static void unbind_tex(uint32_t obj) {
    if (!g_dev) return;
    for (int i = 0; i < 8; i++) if (g_dev->tex_obj[i] == obj) { g_dev->tex_obj[i] = 0; g_dev->tex[i] = NULL; }
}
M(Tex_Release) {
    uint32_t r = com_release(THIS);
    if (!r) {
        Tex *t = com_host(THIS);
        unbind_tex(THIS);
        for (int f = 0; f < 6; f++) for (int l = 0; l < 16; l++) { if (t->lock[f][l]) gfree(t->lock[f][l]); t->lock[f][l] = 0; }
        mtl_release(t->mtl); t->mtl = NULL;
    }
    RET(c, r, 1);
}
M(Tex_GetType) { RET(c, 3, 1); }
M(Tex_SetLOD) { RET(c, 0, 2); }
M(Tex_GetLOD) { RET(c, 0, 1); }
M(Tex_GetLevelCount) { RET(c, ((Tex *)com_host(THIS))->levels, 1); }
static void level_desc(Tex *t, uint32_t l, uint32_t p) {
    uint32_t w = lvl_dim(t->w, l), h = lvl_dim(t->h, l);
    s32(p, t->fmt); s32(p + 4, 1); s32(p + 8, t->usage); s32(p + 12, t->pool);
    s32(p + 16, fmt_size(t->fmt, w, h)); s32(p + 20, 0); s32(p + 24, w); s32(p + 28, h);
}
M(Tex_GetLevelDesc) { level_desc(com_host(THIS), ARG(c, 1), ARG(c, 2)); RET(c, HR_OK, 3); }
static uint32_t tex_surface(Tex *t, int face, uint32_t l) {
    if (!t->surf[face][l]) {
        Surf *s = reg(&g_rsurf, calloc(1, sizeof(Surf)));
        s->kind = 0; s->tex = t; s->face = face; s->level = (int)l;
        s->w = lvl_dim(t->w, l); s->h = lvl_dim(t->h, l); s->fmt = t->fmt; s->usage = t->usage;
        s->obj = com_new(vt_surf, s);
        t->surf[face][l] = s->obj;
    }
    com_addref(t->surf[face][l]);
    return t->surf[face][l];
}
M(Tex_GetSurfaceLevel) { s32(ARG(c, 2), tex_surface(com_host(THIS), 0, ARG(c, 1))); RET(c, HR_OK, 3); }
static uint32_t tex_lock(Tex *t, int face, uint32_t l, uint32_t locked, uint32_t rect) {
    uint32_t w = lvl_dim(t->w, l), h = lvl_dim(t->h, l), pitch = fmt_pitch(t->fmt, w);
    if (!t->lock[face][l]) t->lock[face][l] = galloc(fmt_size(t->fmt, w, h) + 64, 1);
    uint32_t base = t->lock[face][l];
    if (rect) {
        uint32_t x = g32(rect), y = g32(rect + 4);
        if (fmt_is_dxt(t->fmt)) base += (y / 4) * pitch + (x / 4) * (t->fmt == D3DFMT_DXT1 ? 8 : 16);
        else base += y * pitch + x * (uint32_t)fmt_bpp(t->fmt) / 8;
    }
    s32(locked, pitch); s32(locked + 4, base);
    return HR_OK;
}
M(Tex_LockRect) { RET(c, tex_lock(com_host(THIS), 0, ARG(c, 1), ARG(c, 2), ARG(c, 3)), 5); }
M(Tex_UnlockRect) { mtl_upload(g_dev, com_host(THIS), 0, (int)ARG(c, 1)); RET(c, HR_OK, 2); }
M(Tex_AddDirtyRect) { RET(c, HR_OK, 2); }
M(Cube_GetLevelDesc) { level_desc(com_host(THIS), ARG(c, 1), ARG(c, 2)); RET(c, HR_OK, 3); }
M(Cube_GetCubeMapSurface) { s32(ARG(c, 3), tex_surface(com_host(THIS), (int)ARG(c, 1), ARG(c, 2))); RET(c, HR_OK, 4); }
M(Cube_LockRect) { RET(c, tex_lock(com_host(THIS), (int)ARG(c, 1), ARG(c, 2), ARG(c, 3), ARG(c, 4)), 6); }
M(Cube_UnlockRect) { mtl_upload(g_dev, com_host(THIS), (int)ARG(c, 1), (int)ARG(c, 2)); RET(c, HR_OK, 3); }
M(Cube_AddDirtyRect) { RET(c, HR_OK, 3); }

/* ---- surfaces ---- */
M(Surf_Release) { RET(c, com_release(THIS), 1); }
M(Surf_GetContainer) {
    Surf *s = com_host(THIS);
    if (s->tex) { s32(ARG(c, 2), s->tex->obj); com_addref(s->tex->obj); RET(c, HR_OK, 3); }
    else { s32(ARG(c, 2), g_dev->obj); com_addref(g_dev->obj); RET(c, HR_OK, 3); }
}
M(Surf_GetDesc) {
    Surf *s = com_host(THIS);
    uint32_t p = ARG(c, 1);
    s32(p, s->fmt); s32(p + 4, 1); s32(p + 8, s->kind == 2 ? D3DUSAGE_DEPTHSTENCIL : s->kind == 1 || s->kind == 4 ? D3DUSAGE_RENDERTARGET : s->usage);
    s32(p + 12, s->kind == 3 ? 2 : 0); s32(p + 16, fmt_size(s->fmt, s->w, s->h)); s32(p + 20, 0); s32(p + 24, s->w); s32(p + 28, s->h);
    RET(c, HR_OK, 2);
}
M(Surf_LockRect) {
    Surf *s = com_host(THIS);
    uint32_t locked = ARG(c, 1), rect = ARG(c, 2);
    if (s->tex) { RET(c, tex_lock(s->tex, s->face, (uint32_t)s->level, locked, rect), 4); return; }
    if (!s->mem) s->mem = galloc(fmt_size(s->fmt, s->w, s->h) + 64, 1);
    if (s->mtl && (s->kind == 1 || s->kind == 4)) {   /* read back render target contents */
        uint8_t *tmp = malloc((size_t)s->w * s->h * 4);
        mtl_readback(g_dev, s->mtl, tmp, s->w, s->h);
        tex_unconvert(s->fmt, tmp, gptr(s->mem), fmt_pitch(s->fmt, s->w), s->w, s->h);
        free(tmp);
    }
    uint32_t pitch = fmt_pitch(s->fmt, s->w), base = s->mem;
    if (rect) base += g32(rect + 4) * pitch + g32(rect) * (uint32_t)fmt_bpp(s->fmt) / 8;
    s32(locked, pitch); s32(locked + 4, base);
    RET(c, HR_OK, 4);
}
M(Surf_UnlockRect) {
    Surf *s = com_host(THIS);
    if (s->tex) mtl_upload(g_dev, s->tex, s->face, s->level);
    RET(c, HR_OK, 1);
}

/* ---- buffers ---- */
M(Buf_Release) {
    uint32_t r = com_release(THIS);
    if (!r) { Buf *b = com_host(THIS); if (b->mem) gfree(b->mem); b->mem = 0; }
    RET(c, r, 1);
}
M(VB_GetType) { RET(c, 6, 1); }
M(IB_GetType) { RET(c, 7, 1); }
M(Buf_Lock) {
    Buf *b = com_host(THIS);
    s32(ARG(c, 3), b->mem + ARG(c, 1));
    RET(c, HR_OK, 5);
}
M(Buf_Unlock) { RET(c, HR_OK, 1); }
M(VB_GetDesc) { Buf *b = com_host(THIS); uint32_t p = ARG(c, 1); s32(p, 100); s32(p + 4, 6); s32(p + 8, b->usage); s32(p + 12, b->pool); s32(p + 16, b->size); s32(p + 20, b->fvf); RET(c, HR_OK, 2); }
M(IB_GetDesc) { Buf *b = com_host(THIS); uint32_t p = ARG(c, 1); s32(p, b->fmt); s32(p + 4, 7); s32(p + 8, b->usage); s32(p + 12, b->pool); s32(p + 16, b->size); RET(c, HR_OK, 2); }

/* ===================================================================== vtables */
#define V(n) #n
static void build_vtables(void) {
    static const char *const d3d_n[] = {"QueryInterface", "AddRef", "Release", "RegisterSoftwareDevice", "GetAdapterCount",
        "GetAdapterIdentifier", "GetAdapterModeCount", "EnumAdapterModes", "GetAdapterDisplayMode", "CheckDeviceType",
        "CheckDeviceFormat", "CheckDeviceMultiSampleType", "CheckDepthStencilMatch", "GetDeviceCaps", "GetAdapterMonitor", "CreateDevice"};
    static const GuestFn d3d_f[] = {QueryInterface, AddRef, ReleaseNoFree, NULL, D3D_GetAdapterCount, D3D_GetAdapterIdentifier,
        D3D_GetAdapterModeCount, D3D_EnumAdapterModes, D3D_GetAdapterDisplayMode, D3D_CheckDeviceType, D3D_CheckDeviceFormat,
        D3D_CheckDeviceMultiSampleType, D3D_CheckDepthStencilMatch, D3D_GetDeviceCaps, D3D_GetAdapterMonitor, D3D_CreateDevice};
    vt_d3d = com_vtable("IDirect3D8", d3d_n, d3d_f, 16);

    static const char *const dev_n[] = {"QueryInterface", "AddRef", "Release", "TestCooperativeLevel", "GetAvailableTextureMem",
        "ResourceManagerDiscardBytes", "GetDirect3D", "GetDeviceCaps", "GetDisplayMode", "GetCreationParameters",
        "SetCursorProperties", "SetCursorPosition", "ShowCursor", "CreateAdditionalSwapChain", "Reset", "Present",
        "GetBackBuffer", "GetRasterStatus", "SetGammaRamp", "GetGammaRamp", "CreateTexture", "CreateVolumeTexture",
        "CreateCubeTexture", "CreateVertexBuffer", "CreateIndexBuffer", "CreateRenderTarget", "CreateDepthStencilSurface",
        "CreateImageSurface", "CopyRects", "UpdateTexture", "GetFrontBuffer", "SetRenderTarget", "GetRenderTarget",
        "GetDepthStencilSurface", "BeginScene", "EndScene", "Clear", "SetTransform", "GetTransform", "MultiplyTransform",
        "SetViewport", "GetViewport", "SetMaterial", "GetMaterial", "SetLight", "GetLight", "LightEnable", "GetLightEnable",
        "SetClipPlane", "GetClipPlane", "SetRenderState", "GetRenderState", "BeginStateBlock", "EndStateBlock",
        "ApplyStateBlock", "CaptureStateBlock", "DeleteStateBlock", "CreateStateBlock", "SetClipStatus", "GetClipStatus",
        "GetTexture", "SetTexture", "GetTextureStageState", "SetTextureStageState", "ValidateDevice", "GetInfo",
        "SetPaletteEntries", "GetPaletteEntries", "SetCurrentTexturePalette", "GetCurrentTexturePalette", "DrawPrimitive",
        "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices", "CreateVertexShader",
        "SetVertexShader", "GetVertexShader", "DeleteVertexShader", "SetVertexShaderConstant", "GetVertexShaderConstant",
        "GetVertexShaderDeclaration", "GetVertexShaderFunction", "SetStreamSource", "GetStreamSource", "SetIndices",
        "GetIndices", "CreatePixelShader", "SetPixelShader", "GetPixelShader", "DeletePixelShader", "SetPixelShaderConstant",
        "GetPixelShaderConstant", "GetPixelShaderFunction", "DrawRectPatch", "DrawTriPatch", "DeletePatch"};
    static const GuestFn dev_f[] = {QueryInterface, AddRef, ReleaseNoFree, Dev_TestCooperativeLevel, Dev_GetAvailableTextureMem,
        Dev_ResourceManagerDiscardBytes, Dev_GetDirect3D, Dev_GetDeviceCaps, Dev_GetDisplayMode, Dev_GetCreationParameters,
        Dev_SetCursorProperties, Dev_SetCursorPosition, Dev_ShowCursor, NULL, Dev_Reset, Dev_Present,
        Dev_GetBackBuffer, Dev_GetRasterStatus, Dev_SetGammaRamp, Dev_GetGammaRamp, Dev_CreateTexture, NULL,
        Dev_CreateCubeTexture, Dev_CreateVertexBuffer, Dev_CreateIndexBuffer, Dev_CreateRenderTarget, Dev_CreateDepthStencilSurface,
        Dev_CreateImageSurface, Dev_CopyRects, Dev_UpdateTexture, Dev_GetFrontBuffer, Dev_SetRenderTarget, Dev_GetRenderTarget,
        Dev_GetDepthStencilSurface, Dev_BeginScene, Dev_EndScene, Dev_Clear, Dev_SetTransform, Dev_GetTransform, Dev_MultiplyTransform,
        Dev_SetViewport, Dev_GetViewport, Dev_SetMaterial, Dev_GetMaterial, Dev_SetLight, Dev_GetLight, Dev_LightEnable, Dev_GetLightEnable,
        Dev_SetClipPlane, Dev_GetClipPlane, Dev_SetRenderState, Dev_GetRenderState, Dev_BeginStateBlock, Dev_EndStateBlock,
        Dev_ApplyStateBlock, Dev_CaptureStateBlock, Dev_DeleteStateBlock, Dev_CreateStateBlock, Dev_SetClipStatus, Dev_GetClipStatus,
        Dev_GetTexture, Dev_SetTexture, Dev_GetTextureStageState, Dev_SetTextureStageState, Dev_ValidateDevice, Dev_GetInfo,
        Dev_SetPaletteEntries, Dev_GetPaletteEntries, Dev_SetCurrentTexturePalette, Dev_GetCurrentTexturePalette, Dev_DrawPrimitive,
        Dev_DrawIndexedPrimitive, Dev_DrawPrimitiveUP, Dev_DrawIndexedPrimitiveUP, Dev_ProcessVertices, Dev_CreateVertexShader,
        Dev_SetVertexShader, Dev_GetVertexShader, Dev_DeleteVertexShader, Dev_SetVertexShaderConstant, Dev_GetVertexShaderConstant,
        Dev_GetVertexShaderDeclaration, Dev_GetVertexShaderFunction, Dev_SetStreamSource, Dev_GetStreamSource, Dev_SetIndices,
        Dev_GetIndices, Dev_CreatePixelShader, Dev_SetPixelShader, Dev_GetPixelShader, Dev_DeletePixelShader, Dev_SetPixelShaderConstant,
        Dev_GetPixelShaderConstant, NULL, NULL, NULL, NULL};
    vt_dev = com_vtable("IDirect3DDevice8", dev_n, dev_f, 97);

    static const char *const tex_n[] = {"QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData", "GetPrivateData",
        "FreePrivateData", "SetPriority", "GetPriority", "PreLoad", "GetType", "SetLOD", "GetLOD", "GetLevelCount",
        "GetLevelDesc", "GetSurfaceLevel", "LockRect", "UnlockRect", "AddDirtyRect"};
    static const GuestFn tex_f[] = {QueryInterface, AddRef, Tex_Release, Res_GetDevice, Res_SetPrivateData, Res_GetPrivateData,
        Res_FreePrivateData, Res_SetPriority, Res_GetPriority, Res_PreLoad, Tex_GetType, Tex_SetLOD, Tex_GetLOD, Tex_GetLevelCount,
        Tex_GetLevelDesc, Tex_GetSurfaceLevel, Tex_LockRect, Tex_UnlockRect, Tex_AddDirtyRect};
    vt_tex = com_vtable("IDirect3DTexture8", tex_n, tex_f, 19);
    static const GuestFn cube_f[] = {QueryInterface, AddRef, Tex_Release, Res_GetDevice, Res_SetPrivateData, Res_GetPrivateData,
        Res_FreePrivateData, Res_SetPriority, Res_GetPriority, Res_PreLoad, Tex_GetType, Tex_SetLOD, Tex_GetLOD, Tex_GetLevelCount,
        Cube_GetLevelDesc, Cube_GetCubeMapSurface, Cube_LockRect, Cube_UnlockRect, Cube_AddDirtyRect};
    vt_cube = com_vtable("IDirect3DCubeTexture8", tex_n, cube_f, 19);

    static const char *const surf_n[] = {"QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData", "GetPrivateData",
        "FreePrivateData", "GetContainer", "GetDesc", "LockRect", "UnlockRect"};
    static const GuestFn surf_f[] = {QueryInterface, AddRef, Surf_Release, Res_GetDevice, Res_SetPrivateData, Res_GetPrivateData,
        Res_FreePrivateData, Surf_GetContainer, Surf_GetDesc, Surf_LockRect, Surf_UnlockRect};
    vt_surf = com_vtable("IDirect3DSurface8", surf_n, surf_f, 11);

    static const char *const buf_n[] = {"QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData", "GetPrivateData",
        "FreePrivateData", "SetPriority", "GetPriority", "PreLoad", "GetType", "Lock", "Unlock", "GetDesc"};
    static const GuestFn vb_f[] = {QueryInterface, AddRef, Buf_Release, Res_GetDevice, Res_SetPrivateData, Res_GetPrivateData,
        Res_FreePrivateData, Res_SetPriority, Res_GetPriority, Res_PreLoad, VB_GetType, Buf_Lock, Buf_Unlock, VB_GetDesc};
    static const GuestFn ib_f[] = {QueryInterface, AddRef, Buf_Release, Res_GetDevice, Res_SetPrivateData, Res_GetPrivateData,
        Res_FreePrivateData, Res_SetPriority, Res_GetPriority, Res_PreLoad, IB_GetType, Buf_Lock, Buf_Unlock, IB_GetDesc};
    vt_vb = com_vtable("IDirect3DVertexBuffer8", buf_n, vb_f, 14);
    vt_ib = com_vtable("IDirect3DIndexBuffer8", buf_n, ib_f, 14);
}

void shim_D3D8_Direct3DCreate8(CPU *c) {
    g_trace_d3d = getenv("TRACE_D3D") != NULL;
    if (!vt_d3d) build_vtables();
    if (!g_d3d_obj) g_d3d_obj = com_new(vt_d3d, NULL);
    LOG("[d3d] Direct3DCreate8(%u) -> %08X\n", ARG(c, 0), g_d3d_obj);
    RET(c, g_d3d_obj, 1);
}

/* ===================================================================== save states */
static void save_reg(SSBuf *b, Reg *r, size_t sz) {
    SSW(b, r->n);
    for (int i = 0; i < r->n; i++) { SSW(b, r->p[i]); ssw(b, r->p[i], sz); }
}
void d3d_ss_save(SSBuf *b) {
    int has = g_dev != NULL;
    SSW(b, has);
    if (has) SSW(b, *g_dev);
    save_reg(b, &g_rtex, sizeof(Tex));
    save_reg(b, &g_rsurf, sizeof(Surf));
    save_reg(b, &g_rbuf, sizeof(Buf));
    SSW(b, g_nsb);
    for (int t = 0; t < 1024; t++) {
        if (!g_sb[t]) continue;
        SSW(b, t); SSW(b, g_sb[t]->n);
        ssw(b, g_sb[t]->e, sizeof(SBEntry) * (size_t)g_sb[t]->n);
    }
    int end = -1; SSW(b, end);
}
void d3d_ss_load(SSRd *r) {
    int has = 0;
    SSR(r, has);
    if (has) { Dev saved; SSR(r, saved); if (g_dev) { void *metal = g_dev->metal; *g_dev = saved; g_dev->metal = metal; } }
    int n = 0, uploads = 0;
    /* textures: fields come back; a texture released after the save gets a new Metal texture, and any
       texture whose pixels changed is re-uploaded from its staging copy (restored with guest memory) */
    SSR(r, n);
    for (int i = 0; i < n; i++) {
        void *p; Tex saved; SSR(r, p); SSR(r, saved);
        if (i >= g_rtex.n || g_rtex.p[i] != p) continue;
        Tex *t = p;
        void *m = t->mtl; uint32_t gen = t->gen;
        *t = saved; t->mtl = m;
        int up = gen != saved.gen;
        if (!t->mtl && saved.mtl) {
            t->mtl = mtl_new_texture(g_dev, t->w, t->h, t->levels, t->fmt, (t->usage & D3DUSAGE_RENDERTARGET) != 0,
                                     (t->usage & D3DUSAGE_DEPTHSTENCIL) != 0, t->cube);
            up = 1;
        }
        if (up && t->mtl) {
            for (int f = 0; f < (t->cube ? 6 : 1); f++)
                for (uint32_t l = 0; l < t->levels && l < 16; l++) if (t->lock[f][l]) { mtl_upload(g_dev, t, f, (int)l); uploads++; }
        }
    }
    for (int i = n; i < g_rtex.n; i++) {   /* created after the save: nothing refers to them any more */
        Tex *t = g_rtex.p[i];
        if (t->mtl) { mtl_release(t->mtl); t->mtl = NULL; }
        memset(t->lock, 0, sizeof t->lock);
    }
    SSR(r, n);
    for (int i = 0; i < n; i++) {
        void *p; Surf saved; SSR(r, p); SSR(r, saved);
        if (i >= g_rsurf.n || g_rsurf.p[i] != p) continue;
        Surf *s = p; void *m = s->mtl;
        *s = saved; s->mtl = m;
    }
    for (int i = n; i < g_rsurf.n; i++) {
        Surf *s = g_rsurf.p[i];
        if ((s->kind == 2 || s->kind == 4) && s->mtl) { mtl_release(s->mtl); s->mtl = NULL; }
    }
    SSR(r, n);
    for (int i = 0; i < n; i++) {
        void *p; Buf saved; SSR(r, p); SSR(r, saved);
        if (i < g_rbuf.n && g_rbuf.p[i] == p) *(Buf *)p = saved;
    }
    /* state blocks: replace the current set with the saved one */
    int nsb = 0;
    SSR(r, nsb);
    for (int t = 0; t < 1024; t++) if (g_sb[t]) { free(g_sb[t]->e); free(g_sb[t]); g_sb[t] = NULL; }
    for (;;) {
        int t = -1, cnt = 0;
        if (!SSR(r, t) || t < 0 || t >= 1024 || !SSR(r, cnt) || cnt < 0) break;
        StateBlock *b = calloc(1, sizeof *b);
        b->n = b->cap = cnt;
        b->e = malloc(sizeof(SBEntry) * (size_t)(cnt ? cnt : 1));
        ssr(r, b->e, sizeof(SBEntry) * (size_t)cnt);
        g_sb[t] = b;
    }
    if (nsb > g_nsb) g_nsb = nsb;   /* never hand out a token the loaded state may still hold */
    if (g_dev) mtl_set_rendertarget(g_dev);
    LOG("[ss] d3d: %d textures, %d surfaces, %d buffers restored, %d texture levels re-uploaded\n", g_rtex.n, g_rsurf.n, g_rbuf.n, uploads);
}
