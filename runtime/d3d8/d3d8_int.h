/* Direct3D 8 on Metal: shared definitions. */
#pragma once
#include "../com.h"

/* ---- formats ---- */
enum {
    D3DFMT_UNKNOWN = 0, D3DFMT_R8G8B8 = 20, D3DFMT_A8R8G8B8 = 21, D3DFMT_X8R8G8B8 = 22, D3DFMT_R5G6B5 = 23,
    D3DFMT_X1R5G5B5 = 24, D3DFMT_A1R5G5B5 = 25, D3DFMT_A4R4G4B4 = 26, D3DFMT_A8 = 28, D3DFMT_X4R4G4B4 = 30,
    D3DFMT_P8 = 41, D3DFMT_L8 = 50, D3DFMT_A8L8 = 51, D3DFMT_A4L4 = 52, D3DFMT_V8U8 = 60,
    D3DFMT_D16_LOCKABLE = 70, D3DFMT_D32 = 71, D3DFMT_D15S1 = 73, D3DFMT_D24S8 = 75, D3DFMT_D24X8 = 77,
    D3DFMT_D24X4S4 = 79, D3DFMT_D16 = 80, D3DFMT_INDEX16 = 101, D3DFMT_INDEX32 = 102,
    D3DFMT_DXT1 = 0x31545844, D3DFMT_DXT2 = 0x32545844, D3DFMT_DXT3 = 0x33545844,
    D3DFMT_DXT4 = 0x34545844, D3DFMT_DXT5 = 0x35545844, D3DFMT_UYVY = 0x59565955, D3DFMT_YUY2 = 0x32595559,
};
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_INVALIDCALL  0x8876086Cu
#define D3DUSAGE_RENDERTARGET 1u
#define D3DUSAGE_DEPTHSTENCIL 2u
#define D3DUSAGE_DYNAMIC 0x200u

/* FVF */
#define FVF_POSMASK 0x00Eu
#define FVF_XYZ 0x002u
#define FVF_XYZRHW 0x004u
#define FVF_NORMAL 0x010u
#define FVF_PSIZE 0x020u
#define FVF_DIFFUSE 0x040u
#define FVF_SPECULAR 0x080u

/* render state slots we index directly */
enum {
    RS_ZENABLE = 7, RS_FILLMODE = 8, RS_SHADEMODE = 9, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15, RS_SRCBLEND = 19,
    RS_DESTBLEND = 20, RS_CULLMODE = 22, RS_ZFUNC = 23, RS_ALPHAREF = 24, RS_ALPHAFUNC = 25, RS_DITHERENABLE = 26,
    RS_ALPHABLENDENABLE = 27, RS_FOGENABLE = 28, RS_SPECULARENABLE = 29, RS_FOGCOLOR = 34, RS_FOGTABLEMODE = 35,
    RS_FOGSTART = 36, RS_FOGEND = 37, RS_FOGDENSITY = 38, RS_ZBIAS = 47, RS_RANGEFOGENABLE = 48, RS_STENCILENABLE = 52,
    RS_STENCILFAIL = 53, RS_STENCILZFAIL = 54, RS_STENCILPASS = 55, RS_STENCILFUNC = 56, RS_STENCILREF = 57,
    RS_STENCILMASK = 58, RS_STENCILWRITEMASK = 59, RS_TEXTUREFACTOR = 60, RS_CLIPPING = 136, RS_LIGHTING = 137,
    RS_AMBIENT = 139, RS_FOGVERTEXMODE = 140, RS_COLORVERTEX = 141, RS_LOCALVIEWER = 142, RS_NORMALIZENORMALS = 143,
    RS_DIFFUSEMATERIALSOURCE = 145, RS_SPECULARMATERIALSOURCE = 146, RS_AMBIENTMATERIALSOURCE = 147,
    RS_EMISSIVEMATERIALSOURCE = 148, RS_VERTEXBLEND = 151, RS_POINTSIZE = 154, RS_POINTSPRITEENABLE = 156,
    RS_COLORWRITEENABLE = 168, RS_BLENDOP = 171,
};
/* texture stage state */
enum {
    TSS_COLOROP = 1, TSS_COLORARG1 = 2, TSS_COLORARG2 = 3, TSS_ALPHAOP = 4, TSS_ALPHAARG1 = 5, TSS_ALPHAARG2 = 6,
    TSS_TEXCOORDINDEX = 11, TSS_ADDRESSU = 13, TSS_ADDRESSV = 14, TSS_BORDERCOLOR = 15, TSS_MAGFILTER = 16,
    TSS_MINFILTER = 17, TSS_MIPFILTER = 18, TSS_MIPMAPLODBIAS = 19, TSS_MAXMIPLEVEL = 20, TSS_MAXANISOTROPY = 21,
    TSS_TEXTURETRANSFORMFLAGS = 24, TSS_ADDRESSW = 25, TSS_COLORARG0 = 26, TSS_ALPHAARG0 = 27, TSS_RESULTARG = 28,
};
#define MAX_STAGES 4

/* ---- resources ---- */


typedef struct Tex {
    uint32_t obj;
    int cube;
    uint32_t w, h, levels, fmt, usage, pool;
    void *mtl;                  /* id<MTLTexture> (retained) */
    uint32_t lock[6][16];       /* guest staging per face/level, D3D layout */
    uint32_t surf[6][16];       /* guest surface objects */
    int dirty;
    int hud;                    /* created while loading logan_hud.all */
    char src[24];               /* .all file the texture was created from */
} Tex;

typedef struct Surf {
    uint32_t obj;
    Tex *tex; int face, level;  /* texture-backed surface */
    uint32_t w, h, fmt, usage;
    int kind;                   /* 0 = texture level, 1 = backbuffer, 2 = depth, 3 = image (sysmem), 4 = render target */
    uint32_t mem;               /* guest storage for kinds 3/4 */
    void *mtl;                  /* own Metal texture for kinds 1/2/4 (borrowed for backbuffer) */
} Surf;

typedef struct Buf {
    uint32_t obj;
    uint32_t size, usage, fmt, pool, fvf;
    uint32_t mem;               /* guest memory backing the buffer */
} Buf;

typedef struct VShader {
    uint32_t handle;
    uint32_t decl[64]; int ndecl;   /* raw declaration tokens */
    uint32_t *code; int ncode;      /* vs bytecode (NULL = fixed function with decl) */
    void *pso_fn;                   /* id<MTLFunction> for translated shader */
    uint32_t fvf_equiv;             /* FVF derived from decl for fixed-function path */
} VShader;

/* Matrices kept in D3D row-major order */
typedef struct { float m[16]; } Mat;

typedef struct Light { uint32_t type; float diffuse[4], specular[4], ambient[4], pos[3], dir[3], range, falloff, att0, att1, att2, theta, phi; } Light;

typedef struct Dev {
    uint32_t obj;
    uint32_t bbw, bbh, bbfmt, depthfmt;
    uint32_t backbuf_surf, depth_surf;
    Surf *rt, *ds;
    uint32_t rs[256];
    uint32_t tss[8][32];
    Mat view, proj, world[4], texm[8];
    struct { uint32_t x, y, w, h; float minz, maxz; } vp;
    float material[17];
    Light lights[8]; int light_on[8];
    Tex *tex[8];
    uint32_t tex_obj[8];
    uint32_t vs;                          /* FVF or shader handle */
    struct { uint32_t vb_obj; Buf *vb; uint32_t stride; } stream[16];
    uint32_t ib_obj; Buf *ib; uint32_t base_vertex;
    float vsc[96][4];
    uint32_t ps;
    uint32_t gamma[768];
    int in_scene;
    void *metal;                          /* MetalState* (ObjC) */
} Dev;

extern Dev *g_dev;

/* texconv.c */
int fmt_bpp(uint32_t f);
int fmt_is_dxt(uint32_t f);
uint32_t fmt_pitch(uint32_t f, uint32_t w);
uint32_t fmt_size(uint32_t f, uint32_t w, uint32_t h);
int fmt_host_bpp(uint32_t f);
void tex_convert(uint32_t f, const uint8_t *src, uint32_t spitch, uint8_t *dst, uint32_t w, uint32_t h);
void tex_unconvert(uint32_t f, const uint8_t *src, uint8_t *dst, uint32_t dpitch, uint32_t w, uint32_t h);

/* d3d8_metal.m */
void mtl_init(Dev *d);
void *mtl_new_texture(Dev *d, uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt, int rt, int depth, int cube);
void mtl_upload(Dev *d, Tex *t, int face, int level);
void mtl_readback(Dev *d, void *mtltex, uint8_t *dst_bgra, uint32_t w, uint32_t h);
void mtl_release(void *obj);
void mtl_clear(Dev *d, uint32_t flags, uint32_t color, float z, uint32_t stencil);
void mtl_draw(Dev *d, uint32_t prim, uint32_t nverts, const uint8_t *verts, uint32_t stride,
              const void *idx, uint32_t nidx, int idx32, int32_t base_vertex);
void mtl_present(Dev *d);
void mtl_set_rendertarget(Dev *d);
void mtl_copy_surface(Dev *d, Surf *src, Surf *dst, int sx, int sy, int w, int h, int dx, int dy);
uint32_t vs_create(Dev *d, const uint32_t *decl, const uint32_t *code);
VShader *vs_get(uint32_t handle);
void vs_delete(uint32_t handle);
