/* Direct3D 8 rendering on Metal: frames, pipelines, draws, presentation. */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include "d3d8_int.h"
#include "d3d8_shaders.h"
#include "../platform.h"
#include "../touch.h"
#include "../icons.h"

#define RING_SIZE (32u << 20)
#define FRAMES 3

/* Small open-addressing cache: 64-bit key -> retained ObjC object (no per-lookup allocations). */
typedef struct { uint64_t k; void *v; } CEnt;
typedef struct { CEnt e[8192]; } Cache;
static void *cache_get(Cache *c, uint64_t k) {
    k |= 1;   /* 0 marks an empty slot */
    for (uint32_t i = (uint32_t)(k * 0x9E3779B97F4A7C15ull >> 51), n = 0; n < 8192; i = (i + 1) & 8191, n++) {
        if (c->e[i].k == k) return c->e[i].v;
        if (!c->e[i].k) return NULL;
    }
    return NULL;
}
static void cache_put(Cache *c, uint64_t k, id obj) {
    k |= 1;
    for (uint32_t i = (uint32_t)(k * 0x9E3779B97F4A7C15ull >> 51), n = 0; n < 8192; i = (i + 1) & 8191, n++)
        if (!c->e[i].k) { c->e[i].k = k; c->e[i].v = (__bridge_retained void *)obj; return; }
}

@interface MS : NSObject { @public
    id<MTLDevice> dev;
    id<MTLCommandQueue> q;
    CAMetalLayer *layer;
    SDL_MetalView view;
    id<MTLLibrary> lib;
    id<MTLTexture> white, bb_color, bb_depth, gamma, icons;
    id<MTLTexture> cur_color, cur_depth;
    id<MTLRenderPipelineState> blit_pso, ui_pso;
    id<MTLSamplerState> blit_samp, icon_samp;
    id<MTLCommandBuffer> cmd;
    id<MTLRenderCommandEncoder> enc;
    int clear_pending; uint32_t clear_flags; MTLClearColor cc; double cz; uint32_t cs;
    id<MTLBuffer> ring[FRAMES]; uint32_t ring_off; int ring_idx;
    dispatch_semaphore_t sem;
    Cache *pso, *dss, *samp, *clear_pso;
    uint64_t frame;
}
@end
@implementation MS
@end

static MS *M_(Dev *d) { return (__bridge MS *)d->metal; }

/* ------------------------------------------------------------------ init */
void mtl_init(Dev *d) {
    MS *m = [MS new];
    d->metal = (__bridge_retained void *)m;
    m->dev = MTLCreateSystemDefaultDevice();
    m->q = [m->dev newCommandQueue];
    m->view = SDL_Metal_CreateView((SDL_Window *)plat_window());
    m->layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(m->view);
    m->layer.device = m->dev;
    m->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    m->layer.framebufferOnly = YES;
#if TARGET_OS_OSX
    m->layer.displaySyncEnabled = YES;
#endif
    NSError *err = nil;
    m->lib = [m->dev newLibraryWithSource:[NSString stringWithUTF8String:k_ff_msl] options:nil error:&err];
    if (!m->lib) { LOG("[mtl] shader compile failed: %s\n", err.localizedDescription.UTF8String); exit(5); }
    m->pso = calloc(1, sizeof(Cache)); m->dss = calloc(1, sizeof(Cache));
    m->samp = calloc(1, sizeof(Cache)); m->clear_pso = calloc(1, sizeof(Cache));
    for (int i = 0; i < FRAMES; i++) m->ring[i] = [m->dev newBufferWithLength:RING_SIZE options:MTLResourceStorageModeShared];
    m->sem = dispatch_semaphore_create(FRAMES);

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
    m->white = [m->dev newTextureWithDescriptor:td];
    uint32_t w = 0xFFFFFFFFu;
    [m->white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&w bytesPerRow:4];

    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:d->bbw height:d->bbh mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
    m->bb_color = [m->dev newTextureWithDescriptor:td];
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:d->bbw height:d->bbh mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    m->bb_depth = [m->dev newTextureWithDescriptor:td];

    MTLTextureDescriptor *gd = [MTLTextureDescriptor new];
    gd.textureType = MTLTextureType1D; gd.pixelFormat = MTLPixelFormatRGBA8Unorm; gd.width = 256;
    m->gamma = [m->dev newTextureWithDescriptor:gd];
    uint8_t ramp[256 * 4];
    for (int i = 0; i < 256; i++) { ramp[4 * i] = ramp[4 * i + 1] = ramp[4 * i + 2] = (uint8_t)i; ramp[4 * i + 3] = 255; }
    [m->gamma replaceRegion:MTLRegionMake1D(0, 256) mipmapLevel:0 withBytes:ramp bytesPerRow:sizeof ramp];
    for (int i = 0; i < 256; i++) d->gamma[i] = d->gamma[256 + i] = d->gamma[512 + i] = (uint32_t)0;

    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [m->lib newFunctionWithName:@"blit_vs"];
    pd.fragmentFunction = [m->lib newFunctionWithName:@"blit_fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    m->blit_pso = [m->dev newRenderPipelineStateWithDescriptor:pd error:&err];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    m->blit_samp = [m->dev newSamplerStateWithDescriptor:sd];
    MTLRenderPipelineDescriptor *ud = [MTLRenderPipelineDescriptor new];
    ud.vertexFunction = [m->lib newFunctionWithName:@"ui_vs"];
    ud.fragmentFunction = [m->lib newFunctionWithName:@"ui_fs"];
    ud.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    ud.colorAttachments[0].blendingEnabled = YES;
    ud.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    ud.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    ud.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    ud.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    m->ui_pso = [m->dev newRenderPipelineStateWithDescriptor:ud error:&err];
    if (touch_enabled()) {   /* button pictograms: one atlas, mipmapped so small buttons stay smooth */
        const int cell = 256, iw = cell * ICON_COLS, ih = cell * ICON_ROWS;
        uint8_t *px = calloc((size_t)iw * ih, 1);
        icons_render(px, cell);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                     width:iw height:ih mipmapped:YES];
        m->icons = [m->dev newTextureWithDescriptor:td];
        [m->icons replaceRegion:MTLRegionMake2D(0, 0, iw, ih) mipmapLevel:0 withBytes:px bytesPerRow:iw];
        free(px);
        id<MTLCommandBuffer> cb = [m->q commandBuffer];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be generateMipmapsForTexture:m->icons];
        [be endEncoding];
        [cb commit];
        MTLSamplerDescriptor *isd = [MTLSamplerDescriptor new];
        isd.minFilter = isd.magFilter = MTLSamplerMinMagFilterLinear;
        isd.mipFilter = MTLSamplerMipFilterLinear;
        m->icon_samp = [m->dev newSamplerStateWithDescriptor:isd];
    }
    LOG("[mtl] %s, backbuffer %ux%u\n", m->dev.name.UTF8String, d->bbw, d->bbh);
}

/* ------------------------------------------------------------------ textures */
static MTLPixelFormat host_pf(uint32_t fmt) {
    switch (fmt_host_bpp(fmt)) { case 1: return MTLPixelFormatR8Unorm; case 2: return MTLPixelFormatRG8Unorm; default: return MTLPixelFormatBGRA8Unorm; }
}
void *mtl_new_texture(Dev *d, uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt, int rt, int depth, int cube) {
    @autoreleasepool {
    MS *m = M_(d);
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = cube ? MTLTextureTypeCube : MTLTextureType2D;
    td.width = w ? w : 1; td.height = h ? h : 1; td.mipmapLevelCount = levels ? levels : 1;
    if (depth) {
        td.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
    } else {
        td.pixelFormat = host_pf(fmt);
        td.usage = MTLTextureUsageShaderRead | (rt ? MTLTextureUsageRenderTarget : 0);
        td.storageMode = rt ? MTLStorageModePrivate : MTLStorageModeShared;
        if (fmt == D3DFMT_A8) td.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleZero, MTLTextureSwizzleZero, MTLTextureSwizzleZero, MTLTextureSwizzleRed);
        else if (fmt == D3DFMT_L8) td.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne);
        else if (fmt == D3DFMT_A8L8) td.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleGreen);
        else if (rt && (fmt == D3DFMT_X8R8G8B8 || fmt == D3DFMT_R5G6B5))
            td.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne);
    }
    id<MTLTexture> t = [m->dev newTextureWithDescriptor:td];
    return (__bridge_retained void *)t;
    }
}
void mtl_release(void *obj) { if (obj) { id o = (__bridge_transfer id)obj; (void)o; } }

void mtl_upload(Dev *d, Tex *t, int face, int level) {
    @autoreleasepool {
    if (!t->mtl || !t->lock[face][level] || (t->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) return;
    uint32_t w = t->w >> level ? t->w >> level : 1, h = t->h >> level ? t->h >> level : 1;
    int hb = fmt_host_bpp(t->fmt);
    uint8_t *tmp = malloc((size_t)w * h * (size_t)hb + 64);
    tex_convert(t->fmt, gptr(t->lock[face][level]), fmt_pitch(t->fmt, w), tmp, w, h);
    id<MTLTexture> mt = (__bridge id<MTLTexture>)t->mtl;
    [mt replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:(NSUInteger)level slice:(NSUInteger)face
            withBytes:tmp bytesPerRow:w * (uint32_t)hb bytesPerImage:0];
    free(tmp);
    }
}

/* ------------------------------------------------------------------ frame management */
static void end_pass(MS *m) {
    if (m->enc) { [m->enc endEncoding]; m->enc = nil; }
}
static id<MTLCommandBuffer> ensure_cmd(MS *m) {
    if (!m->cmd) {
        dispatch_semaphore_wait(m->sem, DISPATCH_TIME_FOREVER);
        m->cmd = [m->q commandBuffer];
        dispatch_semaphore_t s = m->sem;
        [m->cmd addCompletedHandler:^(id<MTLCommandBuffer> b) { (void)b; dispatch_semaphore_signal(s); }];
        m->ring_idx = (m->ring_idx + 1) % FRAMES;
        m->ring_off = 0;
    }
    return m->cmd;
}
static id<MTLRenderCommandEncoder> ensure_pass(Dev *d) {
    MS *m = M_(d);
    if (m->enc) return m->enc;
    ensure_cmd(m);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = m->cur_color;
    rp.colorAttachments[0].loadAction = (m->clear_pending && (m->clear_flags & 1)) ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.colorAttachments[0].clearColor = m->cc;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (m->cur_depth) {
        rp.depthAttachment.texture = m->cur_depth;
        rp.depthAttachment.loadAction = (m->clear_pending && (m->clear_flags & 2)) ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.clearDepth = m->cz;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.stencilAttachment.texture = m->cur_depth;
        rp.stencilAttachment.loadAction = (m->clear_pending && (m->clear_flags & 4)) ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.stencilAttachment.clearStencil = m->cs;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
    }
    m->clear_pending = 0;
    m->enc = [m->cmd renderCommandEncoderWithDescriptor:rp];
    return m->enc;
}

void mtl_set_rendertarget(Dev *d) {
    @autoreleasepool {
    MS *m = M_(d);
    end_pass(m);
    Surf *rt = d->rt, *ds = d->ds;
    if (!rt || rt->kind == 1) m->cur_color = m->bb_color;
    else if (rt->kind == 0) m->cur_color = (__bridge id<MTLTexture>)rt->tex->mtl;
    else m->cur_color = (__bridge id<MTLTexture>)rt->mtl;
    if (!ds) m->cur_depth = nil;
    else if (ds->kind == 2 && ds->w == d->bbw && ds->h == d->bbh && !ds->mtl) m->cur_depth = m->bb_depth;
    else if (ds->tex) m->cur_depth = (__bridge id<MTLTexture>)ds->tex->mtl;
    else m->cur_depth = ds->mtl ? (__bridge id<MTLTexture>)ds->mtl : m->bb_depth;
    if (m->cur_depth && (m->cur_depth.width != m->cur_color.width || m->cur_depth.height != m->cur_color.height)) m->cur_depth = nil;
    }
}

static uint32_t ring_alloc(MS *m, const void *src, uint32_t n) {
    uint32_t off = (m->ring_off + 255) & ~255u;
    if (off + n > RING_SIZE) { LOG("[mtl] ring overflow (%u bytes)\n", n); return 0xFFFFFFFFu; }
    memcpy((uint8_t *)m->ring[m->ring_idx].contents + off, src, n);
    m->ring_off = off + n;
    return off;
}

static int full_viewport(Dev *d) {
    MS *m = M_(d);
    return d->vp.x == 0 && d->vp.y == 0 && d->vp.w >= m->cur_color.width && d->vp.h >= m->cur_color.height;
}

static id<MTLDepthStencilState> clear_dss(MS *m, uint32_t flags) {
    uint64_t k = (uint64_t)(flags | 0x100) << 48;
    id<MTLDepthStencilState> s = (__bridge id<MTLDepthStencilState>)cache_get(m->dss, k);
    if (!s) {
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = (flags & 2) != 0;
        if (flags & 4) {
            MTLStencilDescriptor *st = [MTLStencilDescriptor new];
            st.stencilCompareFunction = MTLCompareFunctionAlways;
            st.depthStencilPassOperation = MTLStencilOperationReplace;
            dd.frontFaceStencil = dd.backFaceStencil = st;
        }
        s = [m->dev newDepthStencilStateWithDescriptor:dd];
        cache_put(m->dss, k, s);
    }
    return s;
}

void mtl_clear(Dev *d, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    @autoreleasepool {
    MS *m = M_(d);
    MTLClearColor cc = MTLClearColorMake(((color >> 16) & 255) / 255.0, ((color >> 8) & 255) / 255.0, (color & 255) / 255.0, (color >> 24) / 255.0);
    if (!m->cur_depth) flags &= 1;
    if (full_viewport(d)) {
        end_pass(m);
        m->clear_pending = 1; m->clear_flags = flags; m->cc = cc; m->cz = z; m->cs = stencil;
        return;
    }
    /* viewport-restricted clear: draw a quad */
    id<MTLRenderCommandEncoder> e = ensure_pass(d);
    uint64_t k = (uint64_t)((m->cur_color.pixelFormat << 8) | (m->cur_depth ? 1 : 0) | ((flags & 1) << 1));
    id<MTLRenderPipelineState> p = (__bridge id<MTLRenderPipelineState>)cache_get(m->clear_pso, k);
    if (!p) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [m->lib newFunctionWithName:@"clear_vs"];
        pd.fragmentFunction = [m->lib newFunctionWithName:@"clear_fs"];
        pd.colorAttachments[0].pixelFormat = m->cur_color.pixelFormat;
        pd.colorAttachments[0].writeMask = (flags & 1) ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
        if (m->cur_depth) pd.depthAttachmentPixelFormat = pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        p = [m->dev newRenderPipelineStateWithDescriptor:pd error:nil];
        if (p) cache_put(m->clear_pso, k, p);
    }
    [e setRenderPipelineState:p];
    if (m->cur_depth) { [e setDepthStencilState:clear_dss(m, flags)]; [e setStencilReferenceValue:stencil]; }
    [e setViewport:(MTLViewport){d->vp.x, d->vp.y, d->vp.w, d->vp.h, 0, 1}];
    [e setCullMode:MTLCullModeNone];
    float zz[4] = {z, 0, 0, 0}, col[4] = {(float)cc.red, (float)cc.green, (float)cc.blue, (float)cc.alpha};
    [e setVertexBytes:zz length:16 atIndex:0];
    [e setFragmentBytes:col length:16 atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
}

/* ------------------------------------------------------------------ vertex layout */
typedef struct { uint32_t fvf; int pos_fmt, pos_off, nrm_off, dif_off, spc_off, ntex, tex_off[4], tex_n[4]; uint32_t stride_min; int rhw; } Layout;

static void layout_from_fvf(uint32_t fvf, Layout *L) {
    memset(L, 0, sizeof *L);
    L->fvf = fvf;
    L->nrm_off = L->dif_off = L->spc_off = -1;
    uint32_t off = 0, pm = fvf & FVF_POSMASK;
    L->rhw = pm == FVF_XYZRHW;
    L->pos_off = 0;
    if (pm == FVF_XYZRHW) off = 16; else if (pm == FVF_XYZ) off = 12; else if (pm >= 0x6) off = 12 + 4 * ((pm - 4) / 2);
    if (fvf & FVF_NORMAL) { L->nrm_off = (int)off; off += 12; }
    if (fvf & FVF_PSIZE) off += 4;
    if (fvf & FVF_DIFFUSE) { L->dif_off = (int)off; off += 4; }
    if (fvf & FVF_SPECULAR) { L->spc_off = (int)off; off += 4; }
    int nt = (int)((fvf >> 8) & 15);
    for (int i = 0; i < nt; i++) {
        int code = (int)((fvf >> (16 + 2 * i)) & 3);
        int n = code == 0 ? 2 : code == 1 ? 3 : code == 2 ? 4 : 1;
        if (i < 4) { L->tex_off[i] = (int)off; L->tex_n[i] = n; }
        off += 4u * (uint32_t)n;
    }
    L->ntex = nt > 4 ? 4 : nt;
    L->stride_min = off;
}

static MTLVertexFormat float_fmt(int n) {
    return n == 1 ? MTLVertexFormatFloat : n == 2 ? MTLVertexFormatFloat2 : n == 3 ? MTLVertexFormatFloat3 : MTLVertexFormatFloat4;
}

/* ------------------------------------------------------------------ state objects */
static MTLBlendFactor blendf(uint32_t b, int alpha) {
    switch (b) {
    case 1: return MTLBlendFactorZero; case 2: return MTLBlendFactorOne; case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor; case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha; case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha; case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor; case 11: return MTLBlendFactorSourceAlphaSaturated;
    default: return MTLBlendFactorOne;
    }
    (void)alpha;
}
static MTLCompareFunction cmpf(uint32_t f) {
    static const MTLCompareFunction t[9] = {MTLCompareFunctionAlways, MTLCompareFunctionNever, MTLCompareFunctionLess,
        MTLCompareFunctionEqual, MTLCompareFunctionLessEqual, MTLCompareFunctionGreater, MTLCompareFunctionNotEqual,
        MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways};
    return f < 9 ? t[f] : MTLCompareFunctionAlways;
}
static MTLStencilOperation stencilop(uint32_t o) {
    static const MTLStencilOperation t[9] = {MTLStencilOperationKeep, MTLStencilOperationKeep, MTLStencilOperationZero,
        MTLStencilOperationReplace, MTLStencilOperationIncrementClamp, MTLStencilOperationDecrementClamp,
        MTLStencilOperationInvert, MTLStencilOperationIncrementWrap, MTLStencilOperationDecrementWrap};
    return o < 9 ? t[o] : MTLStencilOperationKeep;
}

static id<MTLRenderPipelineState> get_pso(Dev *d, MS *m, const Layout *L, uint32_t stride) {
    uint32_t *rs = d->rs;
    uint32_t src = rs[RS_SRCBLEND], dst = rs[RS_DESTBLEND];
    if (src == 12) { src = 5; dst = 6; } else if (src == 13) { src = 6; dst = 5; }
    int blend = rs[RS_ALPHABLENDENABLE] != 0;
    uint64_t k = (uint64_t)L->fvf * 2654435761u;
    k ^= ((uint64_t)stride << 32) ^ ((uint64_t)blend << 40) ^ ((uint64_t)(blend ? src : 0) << 41) ^ ((uint64_t)(blend ? dst : 0) << 46)
       ^ ((uint64_t)(blend ? rs[RS_BLENDOP] : 0) << 51) ^ ((uint64_t)(rs[RS_COLORWRITEENABLE] & 15) << 54)
       ^ ((uint64_t)(m->cur_depth != nil) << 58) ^ ((uint64_t)(m->cur_color.pixelFormat & 31) << 59);
    id<MTLRenderPipelineState> p = (__bridge id<MTLRenderPipelineState>)cache_get(m->pso, k);
    if (p) return p;

    MTLFunctionConstantValues *fc = [MTLFunctionConstantValues new];
    bool hn = L->nrm_off >= 0, hd = L->dif_off >= 0, hs = L->spc_off >= 0, rhw = L->rhw;
    int nt = L->ntex;
    [fc setConstantValue:&hn type:MTLDataTypeBool atIndex:0];
    [fc setConstantValue:&hd type:MTLDataTypeBool atIndex:1];
    [fc setConstantValue:&hs type:MTLDataTypeBool atIndex:2];
    [fc setConstantValue:&nt type:MTLDataTypeInt atIndex:3];
    [fc setConstantValue:&rhw type:MTLDataTypeBool atIndex:4];
    NSError *err = nil;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [m->lib newFunctionWithName:@"ff_vs" constantValues:fc error:&err];
    pd.fragmentFunction = [m->lib newFunctionWithName:@"ff_fs"];
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = rhw ? MTLVertexFormatFloat4 : MTLVertexFormatFloat3;
    vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 1;
    if (hn) { vd.attributes[1].format = MTLVertexFormatFloat3; vd.attributes[1].offset = (NSUInteger)L->nrm_off; vd.attributes[1].bufferIndex = 1; }
    if (hd) { vd.attributes[2].format = MTLVertexFormatUChar4Normalized_BGRA; vd.attributes[2].offset = (NSUInteger)L->dif_off; vd.attributes[2].bufferIndex = 1; }
    if (hs) { vd.attributes[3].format = MTLVertexFormatUChar4Normalized_BGRA; vd.attributes[3].offset = (NSUInteger)L->spc_off; vd.attributes[3].bufferIndex = 1; }
    for (int i = 0; i < nt; i++) {
        vd.attributes[4 + i].format = float_fmt(L->tex_n[i]);
        vd.attributes[4 + i].offset = (NSUInteger)L->tex_off[i];
        vd.attributes[4 + i].bufferIndex = 1;
    }
    vd.layouts[1].stride = stride ? stride : 4;
    pd.vertexDescriptor = vd;
    MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
    ca.pixelFormat = m->cur_color.pixelFormat;
    if (blend) {
        ca.blendingEnabled = YES;
        ca.sourceRGBBlendFactor = blendf(src, 0); ca.destinationRGBBlendFactor = blendf(dst, 0);
        ca.sourceAlphaBlendFactor = blendf(src, 1); ca.destinationAlphaBlendFactor = blendf(dst, 1);
        MTLBlendOperation op = MTLBlendOperationAdd;
        switch (rs[RS_BLENDOP]) { case 2: op = MTLBlendOperationSubtract; break; case 3: op = MTLBlendOperationReverseSubtract; break;
            case 4: op = MTLBlendOperationMin; break; case 5: op = MTLBlendOperationMax; break; }
        ca.rgbBlendOperation = ca.alphaBlendOperation = op;
    }
    uint32_t cw = rs[RS_COLORWRITEENABLE];
    ca.writeMask = ((cw & 1) ? MTLColorWriteMaskRed : 0) | ((cw & 2) ? MTLColorWriteMaskGreen : 0) |
                   ((cw & 4) ? MTLColorWriteMaskBlue : 0) | ((cw & 8) ? MTLColorWriteMaskAlpha : 0);
    if (m->cur_depth) pd.depthAttachmentPixelFormat = pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    p = [m->dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p) { LOG("[mtl] pipeline failed (fvf %08X): %s\n", L->fvf, err.localizedDescription.UTF8String); return nil; }
    cache_put(m->pso, k, p);
    return p;
}

static id<MTLDepthStencilState> get_dss(Dev *d, MS *m) {
    uint32_t *rs = d->rs;
    int z = rs[RS_ZENABLE] != 0 && m->cur_depth != nil, st = rs[RS_STENCILENABLE] != 0 && m->cur_depth != nil;
    uint64_t k = (uint64_t)z | ((uint64_t)(z && rs[RS_ZWRITEENABLE]) << 1) | ((uint64_t)(z ? rs[RS_ZFUNC] : 0) << 2);
    if (st) k |= ((uint64_t)1 << 8) | ((uint64_t)rs[RS_STENCILFUNC] << 9) | ((uint64_t)rs[RS_STENCILFAIL] << 13) |
                 ((uint64_t)rs[RS_STENCILZFAIL] << 17) | ((uint64_t)rs[RS_STENCILPASS] << 21) |
                 ((uint64_t)(rs[RS_STENCILMASK] & 255) << 25) | ((uint64_t)(rs[RS_STENCILWRITEMASK] & 255) << 33);
    id<MTLDepthStencilState> s = (__bridge id<MTLDepthStencilState>)cache_get(m->dss, k);
    if (s) return s;
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = z ? cmpf(rs[RS_ZFUNC]) : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = z && rs[RS_ZWRITEENABLE];
    if (st) {
        MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = cmpf(rs[RS_STENCILFUNC]);
        sd.stencilFailureOperation = stencilop(rs[RS_STENCILFAIL]);
        sd.depthFailureOperation = stencilop(rs[RS_STENCILZFAIL]);
        sd.depthStencilPassOperation = stencilop(rs[RS_STENCILPASS]);
        sd.readMask = rs[RS_STENCILMASK] & 255; sd.writeMask = rs[RS_STENCILWRITEMASK] & 255;
        dd.frontFaceStencil = dd.backFaceStencil = sd;
    }
    s = [m->dev newDepthStencilStateWithDescriptor:dd];
    cache_put(m->dss, k, s);
    return s;
}

static id<MTLSamplerState> get_sampler(Dev *d, MS *m, int stage) {
    uint32_t *t = d->tss[stage];
    uint32_t k = (t[TSS_ADDRESSU] & 7) | (t[TSS_ADDRESSV] & 7) << 3 | (t[TSS_ADDRESSW] & 7) << 6 |
                 (t[TSS_MAGFILTER] & 3) << 9 | (t[TSS_MINFILTER] & 3) << 11 | (t[TSS_MIPFILTER] & 3) << 13 |
                 (t[TSS_MAXANISOTROPY] & 15) << 15;
    id<MTLSamplerState> s = (__bridge id<MTLSamplerState>)cache_get(m->samp, k);
    if (s) return s;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    MTLSamplerAddressMode am[6] = {MTLSamplerAddressModeRepeat, MTLSamplerAddressModeRepeat, MTLSamplerAddressModeMirrorRepeat,
                                   MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeClampToZero, MTLSamplerAddressModeMirrorClampToEdge};
    sd.sAddressMode = am[t[TSS_ADDRESSU] < 6 ? t[TSS_ADDRESSU] : 1];
    sd.tAddressMode = am[t[TSS_ADDRESSV] < 6 ? t[TSS_ADDRESSV] : 1];
    sd.rAddressMode = am[t[TSS_ADDRESSW] < 6 ? t[TSS_ADDRESSW] : 1];
    sd.magFilter = t[TSS_MAGFILTER] >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.minFilter = t[TSS_MINFILTER] >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = t[TSS_MIPFILTER] == 0 ? MTLSamplerMipFilterNotMipmapped : t[TSS_MIPFILTER] == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    if (t[TSS_MINFILTER] == 3) sd.maxAnisotropy = t[TSS_MAXANISOTROPY] > 1 ? t[TSS_MAXANISOTROPY] : 1;
    s = [m->dev newSamplerStateWithDescriptor:sd];
    cache_put(m->samp, k, s);
    return s;
}

/* ------------------------------------------------------------------ uniforms */
static void mat_mul(float *r, const float *a, const float *b) {   /* row-major a*b */
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(r, t, 64);
}
static void xform_point(const float *m, const float *p, float *out, float w) {
    for (int j = 0; j < 3; j++) out[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + w * m[12 + j];
}
static void unpack_color(uint32_t c, float *o) { o[0] = ((c >> 16) & 255) / 255.f; o[1] = ((c >> 8) & 255) / 255.f; o[2] = (c & 255) / 255.f; o[3] = (c >> 24) / 255.f; }

static void build_vsu(Dev *d, VSU *u, const Layout *L, MS *m) {
    memset(u, 0, sizeof *u);
    mat_mul(u->wv, d->world[0].m, d->view.m);
    memcpy(u->proj, d->proj.m, 64);
    mat_mul(u->wvp, u->wv, d->proj.m);
    u->vp[0] = (float)d->vp.x; u->vp[1] = (float)d->vp.y; u->vp[2] = (float)d->vp.w; u->vp[3] = (float)d->vp.h;
    u->rt[0] = (float)m->cur_color.width; u->rt[1] = (float)m->cur_color.height;
    unpack_color(d->rs[RS_AMBIENT], u->ambient);
    memcpy(u->mat_dif, d->material, 16); memcpy(u->mat_amb, d->material + 4, 16);
    memcpy(u->mat_spc, d->material + 8, 16); memcpy(u->mat_emi, d->material + 12, 16); u->mat_pow[0] = d->material[16];
    int n = 0;
    for (int i = 0; i < 8; i++) {
        if (!d->light_on[i]) continue;
        Light *l = &d->lights[i];
        memcpy(u->l_dif[n], l->diffuse, 16); memcpy(u->l_spc[n], l->specular, 16); memcpy(u->l_amb[n], l->ambient, 16);
        xform_point(d->view.m, l->pos, u->l_pos[n], 1);
        xform_point(d->view.m, l->dir, u->l_dir[n], 0);
        float len = sqrtf(u->l_dir[n][0] * u->l_dir[n][0] + u->l_dir[n][1] * u->l_dir[n][1] + u->l_dir[n][2] * u->l_dir[n][2]);
        if (len > 0) for (int k = 0; k < 3; k++) u->l_dir[n][k] /= len;
        u->l_att[n][0] = l->att0; u->l_att[n][1] = l->att1; u->l_att[n][2] = l->att2; u->l_att[n][3] = l->range > 0 ? l->range : 1e30f;
        u->l_spot[n][0] = l->falloff; u->l_spot[n][1] = cosf(l->theta * 0.5f); u->l_spot[n][2] = cosf(l->phi * 0.5f); u->l_spot[n][3] = (float)l->type;
        n++;
    }
    uint32_t *rs = d->rs;
    u->cfg0[0] = n; u->cfg0[1] = rs[RS_LIGHTING] != 0 && !L->rhw; u->cfg0[2] = rs[RS_NORMALIZENORMALS] != 0; u->cfg0[3] = rs[RS_LOCALVIEWER] != 0;
    u->cfg1[0] = rs[RS_COLORVERTEX] != 0; u->cfg1[1] = (int)rs[RS_DIFFUSEMATERIALSOURCE]; u->cfg1[2] = (int)rs[RS_SPECULARMATERIALSOURCE]; u->cfg1[3] = (int)rs[RS_AMBIENTMATERIALSOURCE];
    u->cfg2[0] = (int)rs[RS_EMISSIVEMATERIALSOURCE]; u->cfg2[1] = rs[RS_SPECULARENABLE] != 0;
    int fogmode = 0;
    if (rs[RS_FOGENABLE]) {
        if (rs[RS_FOGTABLEMODE]) fogmode = (int)rs[RS_FOGTABLEMODE];
        else if (L->rhw || !rs[RS_FOGVERTEXMODE]) fogmode = 4;
        else fogmode = (int)rs[RS_FOGVERTEXMODE];
    }
    u->cfg2[2] = fogmode; u->cfg2[3] = rs[RS_RANGEFOGENABLE] != 0;
    memcpy(&u->fog[0], &rs[RS_FOGSTART], 4); memcpy(&u->fog[1], &rs[RS_FOGEND], 4); memcpy(&u->fog[2], &rs[RS_FOGDENSITY], 4);
    memcpy(&u->fog[3], &rs[RS_POINTSIZE], 4);
    for (int s = 0; s < 4; s++) {
        uint32_t tci = d->tss[s][TSS_TEXCOORDINDEX];
        u->tci[s] = (int)((tci & 0xFFFF) | ((tci >> 16) & 0xF) << 16);
        u->ttf[s] = (int)d->tss[s][TSS_TEXTURETRANSFORMFLAGS];
        memcpy(u->texm[s], d->texm[s].m, 64);
    }
}

static void build_fsu(Dev *d, FSU *u) {
    memset(u, 0, sizeof *u);
    for (int s = 0; s < 4; s++) {
        uint32_t *t = d->tss[s];
        u->cop[s][0] = (int)t[TSS_COLOROP]; u->cop[s][1] = (int)t[TSS_COLORARG1]; u->cop[s][2] = (int)t[TSS_COLORARG2]; u->cop[s][3] = (int)t[TSS_COLORARG0];
        u->aop[s][0] = (int)t[TSS_ALPHAOP]; u->aop[s][1] = (int)t[TSS_ALPHAARG1]; u->aop[s][2] = (int)t[TSS_ALPHAARG2]; u->aop[s][3] = (int)t[TSS_ALPHAARG0];
        u->res[s][0] = (int)t[TSS_RESULTARG]; u->res[s][1] = (t[TSS_TEXTURETRANSFORMFLAGS] & 256) != 0; u->res[s][2] = d->tex[s] != NULL && d->tex[s]->mtl != NULL;
    }
    unpack_color(d->rs[RS_TEXTUREFACTOR], u->tfactor);
    unpack_color(d->rs[RS_FOGCOLOR], u->fogcolor);
    u->flags[0] = d->rs[RS_ALPHATESTENABLE] != 0; u->flags[1] = (int)d->rs[RS_ALPHAFUNC];
    u->flags[2] = d->rs[RS_SPECULARENABLE] != 0; u->flags[3] = d->rs[RS_FOGENABLE] != 0;
    u->aref[0] = (d->rs[RS_ALPHAREF] & 255) / 255.f;
}

/* ------------------------------------------------------------------ draw */
static uint64_t g_draws, g_unsupported_vs;

void mtl_draw(Dev *d, uint32_t prim, uint32_t nverts, const uint8_t *verts, uint32_t stride,
              const void *idx, uint32_t nidx, int idx32, int32_t base_vertex) {
    @autoreleasepool {
    MS *m = M_(d);
    Layout L;
    uint32_t fvf = d->vs;
    if (fvf & 1) {
        VShader *vs = vs_get(fvf);
        if (!vs) return;
        if (vs->code) { if (!g_unsupported_vs++) LOG("[mtl] programmable vertex shaders not yet supported; skipping those draws\n"); return; }
        fvf = vs->fvf_equiv;
    }
    layout_from_fvf(fvf, &L);
    if (!stride) stride = L.stride_min;
    if (!nverts) return;
    static long census = -2;
    if (census == -2) census = getenv("CENSUS_FRAME") ? atol(getenv("CENSUS_FRAME")) : -1;
    if (census >= 0 && (long)m->frame == census) {   /* one-frame census: screen bbox of every draw */
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, wvp[16], tmp[16];
        mat_mul(tmp, d->world[0].m, d->view.m); mat_mul(wvp, tmp, d->proj.m);
        for (uint32_t i = 0; i < nverts; i++) {
            const float *p = (const float *)(verts + i * stride);
            float sx, sy;
            if (L.rhw) { sx = p[0]; sy = p[1]; }
            else {
                float cx = p[0] * wvp[0] + p[1] * wvp[4] + p[2] * wvp[8] + wvp[12];
                float cy = p[0] * wvp[1] + p[1] * wvp[5] + p[2] * wvp[9] + wvp[13];
                float cw = p[0] * wvp[3] + p[1] * wvp[7] + p[2] * wvp[11] + wvp[15];
                if (cw <= 1e-6f) continue;
                sx = d->vp.x + (cx / cw * 0.5f + 0.5f) * d->vp.w;
                sy = d->vp.y + (0.5f - cy / cw * 0.5f) * d->vp.h;
            }
            x0 = fminf(x0, sx); x1 = fmaxf(x1, sx); y0 = fminf(y0, sy); y1 = fmaxf(y1, sy);
        }
        LOG("[census] fvf=%08X rhw=%d n=%u tex=%08X src=%s vp=(%u,%u %ux%u) proj11=%.0f screen=(%.0f,%.0f)-(%.0f,%.0f)\n", fvf, L.rhw, nverts,
            d->tex_obj[0], d->tex[0] ? d->tex[0]->src : "-", d->vp.x, d->vp.y, d->vp.w, d->vp.h, d->proj.m[11], x0, y0, x1, y1);
    }
    static int trace2d = -1;
    if (trace2d < 0) trace2d = getenv("TRACE_2D") != NULL;
    if (trace2d && !L.rhw && d->tex[0] && d->tex[0]->hud && (m->frame % 120) == 0) {   /* HUD quads: bbox in UI units */
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (uint32_t i = 0; i < nverts; i++) {
            const float *p = (const float *)(verts + i * stride);
            x0 = fminf(x0, p[0]); x1 = fmaxf(x1, p[0]); y0 = fminf(y0, p[1]); y1 = fmaxf(y1, p[1]);
        }
        LOG("[hud3d] frame %llu tex=%08X vp=(%u,%u %ux%u) worldT=(%.1f,%.1f,%.1f) proj=(%.3f %.3f | %.3f %.3f)\n", m->frame, d->tex_obj[0],
            d->vp.x, d->vp.y, d->vp.w, d->vp.h, d->world[0].m[12], d->world[0].m[13], d->world[0].m[14],
            d->proj.m[0], d->proj.m[5], d->proj.m[8], d->proj.m[9]);
    }
    if (trace2d && !L.rhw && (m->frame % 120) == 0 && d->proj.m[11] == 0.f && d->proj.m[15] == 1.f && d->proj.m[5] < -0.004f && d->proj.m[5] > -0.005f) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (uint32_t i = 0; i < nverts; i++) {
            const float *p = (const float *)(verts + i * stride);
            x0 = fminf(x0, p[0]); x1 = fmaxf(x1, p[0]); y0 = fminf(y0, p[1]); y1 = fmaxf(y1, p[1]);
        }
        x0 += d->world[0].m[12]; x1 += d->world[0].m[12]; y0 += d->world[0].m[13]; y1 += d->world[0].m[13];
        if (y1 < 120) LOG("[ui] frame %llu tex=%08X src=%s box=(%.0f,%.0f)-(%.0f,%.0f)\n", m->frame, d->tex_obj[0],
                          d->tex[0] ? d->tex[0]->src : "-", x0, y0, x1, y1);
    }
    if (0 && trace2d && !L.rhw && (m->frame % 300) == 0 && d->proj.m[11] == 0.f && d->proj.m[15] == 1.f) {   /* ortho 2D */
        const float *p0 = (const float *)verts;
        LOG("[2d-ortho] frame %llu tex=%08X n=%u v0=(%.1f,%.1f,%.2f) world=(%.2f %.2f | %.1f %.1f) proj=(%.5f %.5f | %.3f %.3f)\n",
            m->frame, d->tex_obj[0], nverts, p0[0], p0[1], p0[2], d->world[0].m[0], d->world[0].m[5], d->world[0].m[12], d->world[0].m[13],
            d->proj.m[0], d->proj.m[5], d->proj.m[12], d->proj.m[13]);
    }
    if (trace2d && L.rhw && (m->frame % 300) == 0) {   /* screen-space draws: log bounding boxes */
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (uint32_t i = 0; i < nverts; i++) {
            const float *p = (const float *)(verts + i * stride);
            x0 = fminf(x0, p[0]); x1 = fmaxf(x1, p[0]); y0 = fminf(y0, p[1]); y1 = fmaxf(y1, p[1]);
        }
        LOG("[2d] frame %llu tex=%08X n=%u box=(%.0f,%.0f)-(%.0f,%.0f)\n", m->frame, d->tex_obj[0], nverts, x0, y0, x1, y1);
    }
    id<MTLRenderCommandEncoder> e = ensure_pass(d);
    id<MTLRenderPipelineState> p = get_pso(d, m, &L, stride);
    if (!p) return;
    uint32_t voff = ring_alloc(m, verts, nverts * stride);
    if (voff == 0xFFFFFFFFu) return;

    /* triangle fans -> lists */
    uint32_t *fan = NULL;
    MTLPrimitiveType pt;
    switch (prim) {
    case 1: pt = MTLPrimitiveTypePoint; break;
    case 2: pt = MTLPrimitiveTypeLine; break;
    case 3: pt = MTLPrimitiveTypeLineStrip; break;
    case 5: pt = MTLPrimitiveTypeTriangleStrip; break;
    default: pt = MTLPrimitiveTypeTriangle; break;
    }
    if (prim == 6) {
        uint32_t n = idx ? nidx : nverts;
        if (n < 3) return;
        fan = malloc(sizeof(uint32_t) * (n - 2) * 3);
        for (uint32_t i = 0; i + 2 < n; i++) {
            uint32_t a = 0, b = i + 1, c2 = i + 2;
            if (idx) {
                a = idx32 ? ((const uint32_t *)idx)[0] : ((const uint16_t *)idx)[0];
                b = idx32 ? ((const uint32_t *)idx)[i + 1] : ((const uint16_t *)idx)[i + 1];
                c2 = idx32 ? ((const uint32_t *)idx)[i + 2] : ((const uint16_t *)idx)[i + 2];
            }
            fan[3 * i] = a; fan[3 * i + 1] = b; fan[3 * i + 2] = c2;
        }
        idx = fan; nidx = (n - 2) * 3; idx32 = 1;
        if (!base_vertex && !idx) base_vertex = 0;
    }

    VSU vu; FSU fu;
    build_vsu(d, &vu, &L, m);
    build_fsu(d, &fu);
    [e setRenderPipelineState:p];
    [e setDepthStencilState:get_dss(d, m)];
    [e setStencilReferenceValue:d->rs[RS_STENCILREF] & 255];
    uint32_t cull = d->rs[RS_CULLMODE];
    [e setFrontFacingWinding:MTLWindingClockwise];
    [e setCullMode:cull == 2 ? MTLCullModeFront : cull == 3 ? MTLCullModeBack : MTLCullModeNone];
    [e setTriangleFillMode:d->rs[RS_FILLMODE] == 2 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    float zb = (float)d->rs[RS_ZBIAS];
    [e setDepthBias:-zb * 2.0f slopeScale:-zb * 0.5f clamp:0];
    uint32_t vw = d->vp.w, vh = d->vp.h;
    if (d->vp.x + vw > m->cur_color.width) vw = (uint32_t)m->cur_color.width - d->vp.x;
    if (d->vp.y + vh > m->cur_color.height) vh = (uint32_t)m->cur_color.height - d->vp.y;
    [e setViewport:(MTLViewport){d->vp.x, d->vp.y, vw, vh, d->vp.minz, d->vp.maxz}];
    [e setVertexBytes:&vu length:sizeof vu atIndex:0];
    [e setVertexBuffer:m->ring[m->ring_idx] offset:voff atIndex:1];
    [e setFragmentBytes:&fu length:sizeof fu atIndex:0];
    for (int s = 0; s < 4; s++) {
        Tex *t = d->tex[s];
        id<MTLTexture> mt = (t && t->mtl && !t->cube) ? (__bridge id<MTLTexture>)t->mtl : m->white;
        [e setFragmentTexture:mt atIndex:(NSUInteger)s];
        [e setFragmentSamplerState:get_sampler(d, m, s) atIndex:(NSUInteger)s];
    }
    if (idx) {
        uint32_t isz = idx32 ? 4 : 2;
        uint32_t ioff = ring_alloc(m, idx, nidx * isz);
        if (ioff != 0xFFFFFFFFu)
            [e drawIndexedPrimitives:pt indexCount:nidx indexType:idx32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                         indexBuffer:m->ring[m->ring_idx] indexBufferOffset:ioff instanceCount:1 baseVertex:base_vertex baseInstance:0];
    } else {
        [e drawPrimitives:pt vertexStart:0 vertexCount:nverts];
    }
    free(fan);
    g_draws++;
    }
}

/* ------------------------------------------------------------------ present */
static void update_gamma(Dev *d, MS *m) {
    static uint16_t last[768];
    uint16_t cur[768];
    memcpy(cur, d->gamma, sizeof cur);
    if (!memcmp(cur, last, sizeof cur)) return;
    memcpy(last, cur, sizeof cur);
    int any = 0;
    for (int i = 0; i < 768; i++) any |= cur[i];
    uint8_t ramp[256 * 4];
    for (int i = 0; i < 256; i++) {
        ramp[4 * i + 0] = any ? (uint8_t)(cur[i] >> 8) : (uint8_t)i;
        ramp[4 * i + 1] = any ? (uint8_t)(cur[256 + i] >> 8) : (uint8_t)i;
        ramp[4 * i + 2] = any ? (uint8_t)(cur[512 + i] >> 8) : (uint8_t)i;
        ramp[4 * i + 3] = 255;
    }
    [m->gamma replaceRegion:MTLRegionMake1D(0, 256) mipmapLevel:0 withBytes:ramp bytesPerRow:sizeof ramp];
}

/* Test dumps: DUMP_EVERY=n frames, or DUMP_SECS=s for one dump per s seconds (named by time, same clock as AUTOKEYS). */
static void dump_frame(Dev *d, MS *m) {
    static int every = -1; static float secs = -1, next;
    if (every < 0) { const char *e = getenv("DUMP_EVERY"); every = e ? atoi(e) : 0; e = getenv("DUMP_SECS"); secs = e ? strtof(e, NULL) : 0; next = secs; }
    char name[64];
    float t = SDL_GetTicks() / 1000.f;
    if (secs > 0 && t >= next) { next = t + secs; snprintf(name, sizeof name, "t%05.1f", t); }
    else if (every && m->frame % (uint64_t)every == 0) snprintf(name, sizeof name, "frame_%05llu", m->frame);
    else return;
    uint32_t w = d->bbw, h = d->bbh;
    uint8_t *px = malloc((size_t)w * h * 4);
    mtl_readback(d, (__bridge void *)m->bb_color, px, w, h);
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.bmp", getenv("DUMP_DIR") ? getenv("DUMP_DIR") : "/tmp", name);
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t sz = 54 + w * h * 4;
        uint8_t hd[54] = {'B', 'M'};
        memcpy(hd + 2, &sz, 4); hd[10] = 54; hd[14] = 40;
        int32_t W = (int32_t)w, H = -(int32_t)h;
        memcpy(hd + 18, &W, 4); memcpy(hd + 22, &H, 4); hd[26] = 1; hd[28] = 32;
        fwrite(hd, 1, 54, f); fwrite(px, 1, (size_t)w * h * 4, f); fclose(f);
    }
    free(px);
}

void mtl_present(Dev *d) {
    MS *m = M_(d);
    @autoreleasepool {
        if (m->clear_pending) ensure_pass(d);
        end_pass(m);
        dump_frame(d, m);
        update_gamma(d, m);
        int w, h;
        plat_window_size(&w, &h);
        if (w > 0 && h > 0) m->layer.drawableSize = CGSizeMake(w, h);
        id<CAMetalDrawable> dr = [m->layer nextDrawable];
        id<MTLCommandBuffer> cb = ensure_cmd(m);
        if (dr) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = dr.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            double sa = (double)d->bbw / d->bbh, da = (double)w / h, vw = w, vh = h;
            if (da > sa) vw = h * sa; else vh = w / sa;
            [e setViewport:(MTLViewport){(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1}];
            [e setRenderPipelineState:m->blit_pso];
            [e setFragmentTexture:m->bb_color atIndex:0];
            [e setFragmentTexture:m->gamma atIndex:1];
            [e setFragmentSamplerState:m->blit_samp atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            UIInst ui[32];
            int nui = touch_overlay(ui, 32, (float)w, (float)h);
            if (nui > 0 && m->ui_pso) {
                float scr[2] = {(float)w, (float)h};
                [e setViewport:(MTLViewport){0, 0, (double)w, (double)h, 0, 1}];
                [e setRenderPipelineState:m->ui_pso];
                [e setVertexBytes:ui length:sizeof(UIInst) * (NSUInteger)nui atIndex:0];
                [e setVertexBytes:scr length:sizeof scr atIndex:1];
                [e setFragmentTexture:m->icons ? m->icons : m->white atIndex:0];
                [e setFragmentSamplerState:m->icon_samp ? m->icon_samp : m->blit_samp atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:(NSUInteger)nui];
            }
            [e endEncoding];
            [cb presentDrawable:dr];
        }
        [cb commit];
        m->cmd = nil;
        m->frame++;
        if (getenv("TRACE_FRAMES") && m->frame % 120 == 0) {
            static uint64_t t_last, d_last;
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t t = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
            if (t_last) LOG("[mtl] frame %llu: %.1f fps, %llu draws/frame\n", m->frame, 120e6 / (double)(t - t_last), (g_draws - d_last) / 120);
            t_last = t; d_last = g_draws;
        }
    }
}

/* ------------------------------------------------------------------ copies / readback */
void mtl_readback(Dev *d, void *mtltex, uint8_t *dst, uint32_t w, uint32_t h) {
    @autoreleasepool {
    MS *m = M_(d);
    end_pass(m);
    id<MTLTexture> t = (__bridge id<MTLTexture>)mtltex;
    id<MTLBuffer> buf = [m->dev newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = ensure_cmd(m);
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:buf destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:(NSUInteger)w * h * 4];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    m->cmd = nil;
    memcpy(dst, buf.contents, (size_t)w * h * 4);
    }
}

void mtl_copy_surface(Dev *d, Surf *src, Surf *dst, int sx, int sy, int w, int h, int dx, int dy) {
    @autoreleasepool {
    /* CPU path: both sides have D3D-layout staging memory (sysmem surfaces or texture levels) */
    uint32_t smem = src->tex ? src->tex->lock[src->face][src->level] : src->mem;
    if (!smem) {
        if (src->kind == 3 || src->tex) return;
        /* GPU source (render target / backbuffer): read back to a temporary sysmem copy */
        Surf tmp = *src;
        tmp.mem = galloc(fmt_size(src->fmt, src->w, src->h), 0);
        uint8_t *bgra = malloc((size_t)src->w * src->h * 4);
        mtl_readback(d, src->kind == 1 ? (__bridge void *)M_(d)->bb_color : src->mtl, bgra, src->w, src->h);
        tex_unconvert(src->fmt, bgra, gptr(tmp.mem), fmt_pitch(src->fmt, src->w), src->w, src->h);
        free(bgra);
        mtl_copy_surface(d, &tmp, dst, sx, sy, w, h, dx, dy);
        gfree(tmp.mem);
        return;
    }
    uint32_t dmem;
    if (dst->tex) {
        Tex *t = dst->tex;
        if (!t->lock[dst->face][dst->level]) t->lock[dst->face][dst->level] = galloc(fmt_size(t->fmt, dst->w, dst->h) + 64, 1);
        dmem = t->lock[dst->face][dst->level];
    } else {
        if (!dst->mem) dst->mem = galloc(fmt_size(dst->fmt, dst->w, dst->h) + 64, 1);
        dmem = dst->mem;
    }
    uint32_t sp = fmt_pitch(src->fmt, src->w), dp = fmt_pitch(dst->fmt, dst->w);
    if (fmt_is_dxt(src->fmt)) {   /* block-aligned copy */
        uint32_t bs = src->fmt == D3DFMT_DXT1 ? 8 : 16;
        for (int y = 0; y < (h + 3) / 4; y++)
            memcpy(gptr(dmem + (uint32_t)(dy / 4 + y) * dp + (uint32_t)(dx / 4) * bs), gptr(smem + (uint32_t)(sy / 4 + y) * sp + (uint32_t)(sx / 4) * bs), (uint32_t)((w + 3) / 4) * bs);
    } else {
        uint32_t bpp = (uint32_t)fmt_bpp(src->fmt) / 8;
        for (int y = 0; y < h; y++)
            memcpy(gptr(dmem + (uint32_t)(dy + y) * dp + (uint32_t)dx * bpp), gptr(smem + (uint32_t)(sy + y) * sp + (uint32_t)sx * bpp), (uint32_t)w * bpp);
    }
    if (dst->tex) mtl_upload(d, dst->tex, dst->face, dst->level);
    }
}
