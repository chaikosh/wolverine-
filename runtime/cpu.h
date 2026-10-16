/* Guest CPU model shared by generated code and the runtime. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t df;                 /* direction flag (0/1) */
    uint32_t fs_base;            /* TEB address */
    double   st[8];              /* x87 registers (double precision) */
    uint32_t top;                /* x87 stack top */
    uint16_t fpu_cw, fpu_sw;     /* control word, status word (C0-C3 bits) */
    uint64_t mm[8];
    union { float f[4]; uint32_t u[4]; uint64_t q[2]; double d[2]; } xmm[8];
    uint32_t mxcsr;
    uint32_t thread_id;
} CPU;

typedef void (*GuestFn)(CPU *);

extern uint8_t *g_mem;            /* base of the 4 GiB guest address space */
extern GuestFn g_imports[];       /* indexed by import number */

void dispatch(CPU *c, uint32_t target);                  /* call guest/host code at address */
void rt_trap(CPU *c, uint32_t addr, const char *what);  /* fatal: unimplemented/invalid */
void rt_cpuid(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d);
uint64_t rt_rdtsc(void);
jmp_buf *rt_setjmp_slot(CPU *c, uint32_t guest_buf, uint32_t esp_after);
void rt_longjmp(CPU *c) __attribute__((noreturn));

typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef float    __attribute__((aligned(1), may_alias)) f32u;
typedef double   __attribute__((aligned(1), may_alias)) f64u;

/* Guest addresses are masked into a 1 GiB window: a wild pointer stays inside guest memory. */
#define GMASK 0x3FFFFFFFu
#define GP(a)    (M + ((uint32_t)(a) & GMASK))
#define RD8(a)   (*(uint8_t *)GP(a))
#define RD16(a)  (*(u16u *)GP(a))
#define RD32(a)  (*(u32u *)GP(a))
#define RD64(a)  (*(u64u *)GP(a))
#define RDF32(a) (*(f32u *)GP(a))
#define RDF64(a) (*(f64u *)GP(a))
#define WR8(a, v)   (*(uint8_t *)GP(a) = (uint8_t)(v))
#define WR16(a, v)  (*(u16u *)GP(a) = (uint16_t)(v))
#define WR32(a, v)  (*(u32u *)GP(a) = (uint32_t)(v))
#define WR64(a, v)  (*(u64u *)GP(a) = (uint64_t)(v))
#define WRF32(a, v) (*(f32u *)GP(a) = (float)(v))
#define WRF64(a, v) (*(f64u *)GP(a) = (double)(v))

#define SPILL() (c->eax = eax, c->ecx = ecx, c->edx = edx, c->ebx = ebx, \
                 c->esp = esp, c->ebp = ebp, c->esi = esi, c->edi = edi)
#define FILL()  (eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, \
                 esp = c->esp, ebp = c->ebp, esi = c->esi, edi = c->edi)
#define PROLOGUE() uint8_t *const M = g_mem; \
    uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, \
             esp = c->esp, ebp = c->ebp, esi = c->esi, edi = c->edi; \
    uint8_t cf = 0, pf = 0, af = 0, zf = 0, sf = 0, of = 0; \
    (void)M; (void)cf; (void)pf; (void)af; (void)zf; (void)sf; (void)of

#define PF8(v) ((uint8_t)(!(__builtin_popcount((uint8_t)(v)) & 1)))

/* ---------------- x87 ---------------- */
#define ST(i) c->st[(c->top + (i)) & 7]
static inline void fpush(CPU *c, double v) { c->top = (c->top - 1) & 7; c->st[c->top] = v; }
static inline void fpop(CPU *c) { c->top = (c->top + 1) & 7; }

static inline void fcom_sw(CPU *c, double a, double b) {
    uint16_t sw = c->fpu_sw & ~0x4500;
    if (isnan(a) || isnan(b)) sw |= 0x4500;
    else if (a < b) sw |= 0x0100;
    else if (a == b) sw |= 0x4000;
    c->fpu_sw = sw;
}
static inline uint16_t fnstsw(CPU *c) { return (uint16_t)((c->fpu_sw & ~0x3800) | ((c->top & 7) << 11)); }

static inline double fround(CPU *c, double v) {
    switch ((c->fpu_cw >> 10) & 3) {
    case 0: return nearbyint(v);   /* host default: round-to-nearest-even */
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
static inline uint16_t fist16(CPU *c, double v) {
    double r = fround(c, v);
    return (r >= -32768.0 && r <= 32767.0) ? (uint16_t)(int16_t)r : 0x8000u;
}
static inline uint32_t fist32(CPU *c, double v) {
    double r = fround(c, v);
    return (r >= -2147483648.0 && r <= 2147483647.0) ? (uint32_t)(int32_t)r : 0x80000000u;
}
static inline uint64_t fist64(CPU *c, double v) {
    double r = fround(c, v);
    return (r >= -9223372036854775808.0 && r < 9223372036854775808.0) ? (uint64_t)(int64_t)r
                                                                       : 0x8000000000000000ull;
}
static inline double ld80(const uint8_t *p) {
    uint64_t mant; uint16_t se;
    memcpy(&mant, p, 8); memcpy(&se, p + 8, 2);
    int e = se & 0x7FFF; double s = (se & 0x8000) ? -1.0 : 1.0;
    if (e == 0 && mant == 0) return s * 0.0;
    if (e == 0x7FFF) return (mant << 1) ? NAN : s * INFINITY;
    return s * ldexp((double)mant, e - 16383 - 63);
}
static inline void st80(uint8_t *p, double v) {
    uint64_t mant = 0; uint16_t se = signbit(v) ? 0x8000 : 0;
    if (isnan(v)) { se |= 0x7FFF; mant = 0xC000000000000000ull; }
    else if (isinf(v)) { se |= 0x7FFF; mant = 0x8000000000000000ull; }
    else if (v != 0) {
        int e; double f = frexp(fabs(v), &e);          /* f in [0.5,1) */
        mant = (uint64_t)ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(p, &mant, 8); memcpy(p + 8, &se, 2);
}
static inline uint16_t fxam(CPU *c) {
    double v = ST(0); uint16_t sw = c->fpu_sw & ~0x4700;
    if (signbit(v)) sw |= 0x0200;
    if (isnan(v)) sw |= 0x0100;
    else if (isinf(v)) sw |= 0x0500;
    else if (v == 0) sw |= 0x4000;
    else if (fpclassify(v) == FP_SUBNORMAL) sw |= 0x4400;
    else sw |= 0x0400;
    return sw;
}

/* ---------------- string operations ---------------- */
static inline void rep_movs(uint8_t *M, uint32_t *esi, uint32_t *edi, uint32_t *ecx, uint32_t df, int sz) {
    uint32_t n = *ecx;
    if (!n) return;
    uint32_t bytes = n * (uint32_t)sz;
    if (!df && (*edi + bytes <= *esi || *esi + bytes <= *edi || *edi < *esi)) {
        memmove(GP(*edi), GP(*esi), bytes);   /* forward copy semantics preserved when dst<src */
        *esi += bytes; *edi += bytes;
    } else {
        int32_t d = df ? -sz : sz;
        while (n--) { memcpy(GP(*edi), GP(*esi), sz); *esi += d; *edi += d; }
    }
    *ecx = 0;
}
static inline void rep_stos(uint8_t *M, uint32_t *edi, uint32_t *ecx, uint32_t df, int sz, uint32_t v) {
    uint32_t n = *ecx;
    if (!n) return;
    if (!df && sz == 1) { memset(GP(*edi), (int)(uint8_t)v, n); *edi += n; }
    else {
        int32_t d = df ? -sz : sz;
        while (n--) { memcpy(GP(*edi), &v, sz); *edi += d; }
    }
    *ecx = 0;
}
