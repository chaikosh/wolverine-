/* D3D8 texture formats -> BGRA8 (or R8 / RG8) for Metal, incl. DXT1-5 decode. */
#include "d3d8_int.h"

int fmt_bpp(uint32_t f) {   /* bits per pixel of the D3D memory layout */
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D32: return 32;
    case D3DFMT_R8G8B8: return 24;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8: case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_V8U8: return 16;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_A4L4: return 8;
    case D3DFMT_DXT1: return 4;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return 8;
    default: return 32;
    }
}
int fmt_is_dxt(uint32_t f) {
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}
uint32_t fmt_pitch(uint32_t f, uint32_t w) {
    if (fmt_is_dxt(f)) return ((w + 3) / 4) * (f == D3DFMT_DXT1 ? 8u : 16u);
    return (w * (uint32_t)fmt_bpp(f) / 8 + 3) & ~3u;
}
uint32_t fmt_size(uint32_t f, uint32_t w, uint32_t h) {
    if (fmt_is_dxt(f)) return fmt_pitch(f, w) * ((h + 3) / 4);
    return fmt_pitch(f, w) * h;
}
/* Host texel layout used for a D3D format: 4 = BGRA8, 1 = R8, 2 = RG8 */
int fmt_host_bpp(uint32_t f) {
    switch (f) { case D3DFMT_A8: case D3DFMT_L8: return 1; case D3DFMT_A8L8: return 2; default: return 4; }
}

static inline uint32_t expand565(uint16_t c) {
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

static void dxt_color_block(const uint8_t *s, uint32_t out[16], int dxt1) {
    uint16_t c0 = (uint16_t)(s[0] | s[1] << 8), c1 = (uint16_t)(s[2] | s[3] << 8);
    uint32_t p[4];
    p[0] = expand565(c0); p[1] = expand565(c1);
    uint32_t r0 = (p[0] >> 16) & 255, g0 = (p[0] >> 8) & 255, b0 = p[0] & 255;
    uint32_t r1 = (p[1] >> 16) & 255, g1 = (p[1] >> 8) & 255, b1 = p[1] & 255;
    if (c0 > c1 || !dxt1) {
        p[2] = 0xFF000000u | ((2 * r0 + r1) / 3) << 16 | ((2 * g0 + g1) / 3) << 8 | (2 * b0 + b1) / 3;
        p[3] = 0xFF000000u | ((r0 + 2 * r1) / 3) << 16 | ((g0 + 2 * g1) / 3) << 8 | (b0 + 2 * b1) / 3;
    } else {
        p[2] = 0xFF000000u | ((r0 + r1) / 2) << 16 | ((g0 + g1) / 2) << 8 | (b0 + b1) / 2;
        p[3] = 0;   /* transparent black */
    }
    uint32_t idx = (uint32_t)s[4] | (uint32_t)s[5] << 8 | (uint32_t)s[6] << 16 | (uint32_t)s[7] << 24;
    for (int i = 0; i < 16; i++) out[i] = p[(idx >> (2 * i)) & 3];
}

static void dxt5_alpha(const uint8_t *s, uint8_t a[16]) {
    uint8_t v[8] = {s[0], s[1]};
    if (s[0] > s[1]) for (int i = 1; i < 7; i++) v[i + 1] = (uint8_t)(((7 - i) * s[0] + i * s[1]) / 7);
    else { for (int i = 1; i < 5; i++) v[i + 1] = (uint8_t)(((5 - i) * s[0] + i * s[1]) / 5); v[6] = 0; v[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)s[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) a[i] = v[(bits >> (3 * i)) & 7];
}

/* Convert one mip level from D3D layout (src, pitch spitch) to host layout (dst, tightly packed). */
void tex_convert(uint32_t f, const uint8_t *src, uint32_t spitch, uint8_t *dst, uint32_t w, uint32_t h) {
    if (fmt_is_dxt(f)) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bs = f == D3DFMT_DXT1 ? 8 : 16;
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                const uint8_t *b = src + by * spitch + bx * bs;
                uint32_t px[16]; uint8_t al[16];
                if (f == D3DFMT_DXT1) dxt_color_block(b, px, 1);
                else {
                    dxt_color_block(b + 8, px, 0);
                    if (f == D3DFMT_DXT2 || f == D3DFMT_DXT3)
                        for (int i = 0; i < 16; i++) { uint8_t n = (b[i / 2] >> (4 * (i & 1))) & 15; al[i] = (uint8_t)(n * 17); }
                    else dxt5_alpha(b, al);
                    for (int i = 0; i < 16; i++) px[i] = (px[i] & 0x00FFFFFFu) | (uint32_t)al[i] << 24;
                }
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        uint32_t X = bx * 4 + (uint32_t)x, Y = by * 4 + (uint32_t)y;
                        if (X < w && Y < h) memcpy(dst + (Y * w + X) * 4, &px[y * 4 + x], 4);
                    }
            }
        return;
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = src + y * spitch;
        uint8_t *d = dst + y * w * (uint32_t)fmt_host_bpp(f);
        switch (f) {
        case D3DFMT_A8R8G8B8: memcpy(d, s, w * 4); break;
        case D3DFMT_X8R8G8B8: for (uint32_t x = 0; x < w; x++) { memcpy(d + 4 * x, s + 4 * x, 3); d[4 * x + 3] = 255; } break;
        case D3DFMT_R8G8B8: for (uint32_t x = 0; x < w; x++) { memcpy(d + 4 * x, s + 3 * x, 3); d[4 * x + 3] = 255; } break;
        case D3DFMT_R5G6B5: for (uint32_t x = 0; x < w; x++) { uint32_t v = expand565((uint16_t)(s[2 * x] | s[2 * x + 1] << 8)); memcpy(d + 4 * x, &v, 4); } break;
        case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
            for (uint32_t x = 0; x < w; x++) {
                uint16_t c = (uint16_t)(s[2 * x] | s[2 * x + 1] << 8);
                uint32_t r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;
                d[4 * x + 0] = (uint8_t)(b << 3 | b >> 2); d[4 * x + 1] = (uint8_t)(g << 3 | g >> 2); d[4 * x + 2] = (uint8_t)(r << 3 | r >> 2);
                d[4 * x + 3] = (f == D3DFMT_X1R5G5B5 || (c & 0x8000)) ? 255 : 0;
            }
            break;
        case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
            for (uint32_t x = 0; x < w; x++) {
                uint16_t c = (uint16_t)(s[2 * x] | s[2 * x + 1] << 8);
                d[4 * x + 0] = (uint8_t)((c & 15) * 17); d[4 * x + 1] = (uint8_t)(((c >> 4) & 15) * 17);
                d[4 * x + 2] = (uint8_t)(((c >> 8) & 15) * 17); d[4 * x + 3] = f == D3DFMT_X4R4G4B4 ? 255 : (uint8_t)(((c >> 12) & 15) * 17);
            }
            break;
        case D3DFMT_A8: case D3DFMT_L8: memcpy(d, s, w); break;
        case D3DFMT_A8L8: memcpy(d, s, w * 2); break;
        default: memset(d, 0xFF, w * 4); break;
        }
    }
}

/* Host BGRA8 -> D3D layout (for reading back render targets into lockable surfaces). */
void tex_unconvert(uint32_t f, const uint8_t *src, uint8_t *dst, uint32_t dpitch, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = src + y * w * 4;
        uint8_t *d = dst + y * dpitch;
        if (f == D3DFMT_A8R8G8B8 || f == D3DFMT_X8R8G8B8) { memcpy(d, s, w * 4); continue; }
        for (uint32_t x = 0; x < w; x++) {
            uint32_t b = s[4 * x], g = s[4 * x + 1], r = s[4 * x + 2], a = s[4 * x + 3];
            uint16_t v;
            if (f == D3DFMT_R5G6B5) v = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
            else if (f == D3DFMT_A4R4G4B4) v = (uint16_t)((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4);
            else v = (uint16_t)((a >> 7) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3);
            d[2 * x] = (uint8_t)v; d[2 * x + 1] = (uint8_t)(v >> 8);
        }
    }
}
