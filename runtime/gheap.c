/* Guest heap: segregated free lists inside the guest address space. */
#include "runtime.h"
#include <pthread.h>
#include <stdlib.h>

#define HDR 16u
#define MAGIC 0x48454150u            /* 'HEAP' */
#define NSMALL 64                    /* 16-byte classes up to 1 KiB */
#define NCLASS (NSMALL + 32)

static uint32_t g_bump = GUEST_HEAP_BASE, g_vbump = GUEST_VA_BASE;
static uint32_t g_free[NCLASS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t class_of(uint32_t size, uint32_t *cap) {
    if (size <= 16u * NSMALL) {
        uint32_t k = (size + 15) / 16;
        if (!k) k = 1;
        *cap = k * 16;
        return k - 1;
    }
    uint32_t k = 32 - (uint32_t)__builtin_clz(size - 1);   /* ceil(log2) */
    *cap = 1u << k;
    return NSMALL + k;
}

void gheap_init(void) {}

uint32_t galloc(uint32_t size, int zero) {
    uint32_t cap, k = class_of(size ? size : 1, &cap);
    pthread_mutex_lock(&g_lock);
    uint32_t p = g_free[k];
    if (p) {
        g_free[k] = g32(p);
    } else {
        if (g_bump + HDR + cap > GUEST_HEAP_END) { pthread_mutex_unlock(&g_lock); return 0; }
        p = g_bump + HDR;
        g_bump += HDR + cap;
    }
    pthread_mutex_unlock(&g_lock);
    s32(p - 16, size); s32(p - 12, MAGIC); s32(p - 8, k); s32(p - 4, cap);
    if (zero) memset(gptr(p), 0, cap);
    return p;
}

void gfree(uint32_t p) {
    if (!p) return;
    if (g32(p - 12) != MAGIC) { LOG("[heap] free of bad pointer %08X\n", p); return; }
    uint32_t k = g32(p - 8);
    s32(p - 12, 0);
    pthread_mutex_lock(&g_lock);
    s32(p, g_free[k]);
    g_free[k] = p;
    pthread_mutex_unlock(&g_lock);
}

uint32_t gsize(uint32_t p) {
    return (p && g32(p - 12) == MAGIC) ? g32(p - 16) : 0xFFFFFFFFu;
}

uint32_t grealloc(uint32_t p, uint32_t size) {
    if (!p) return galloc(size, 0);
    if (g32(p - 12) != MAGIC) return 0;
    if (size <= g32(p - 4)) { s32(p - 16, size); return p; }
    uint32_t n = galloc(size, 0);
    if (!n) return 0;
    memcpy(gptr(n), gptr(p), g32(p - 16));
    gfree(p);
    return n;
}

uint32_t gvalloc(uint32_t size) {
    size = (size + 0xFFFFu) & ~0xFFFFu;
    pthread_mutex_lock(&g_lock);
    uint32_t p = g_vbump;
    if (p + size > GUEST_VA_END) p = 0; else g_vbump += size;
    pthread_mutex_unlock(&g_lock);
    return p;   /* fresh anonymous memory is already zero */
}

uint32_t gstrdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, p = galloc(n, 0);
    memcpy(gptr(p), s, n);
    return p;
}
