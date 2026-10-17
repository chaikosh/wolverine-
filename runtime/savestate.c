/* Quick save / quick load (see savestate.h).

   Why this works for a static recompilation: lifted functions keep guest registers in host locals only
   between calls (SPILL/FILL around every call), so a host call chain carries no guest state of its own.
   If the guest call chain at the moment of loading is the same as when saving, the host frames are the
   same functions at the same call sites, and replacing guest memory + CPU is enough. Present is always
   called from one place in this game, so the check is cheap: compare the return addresses on the stack. */
#include "savestate.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

static atomic_int g_req;
static int g_fb_kind;
static uint64_t g_fb_time;

typedef struct { uint32_t addr, len; uint8_t *data; size_t cap; } Range;
enum { M_K32, M_TIME, M_FILES, M_D3D, M_DS, M_BINK, M_COUNT };
#define MAXCHAIN 256
static struct {
    int valid;
    CPU cpu;
    uint32_t chain[MAXCHAIN][2]; int nchain;
    GHeapState heap;
    Range r[6]; int nr;
    SSBuf mod[M_COUNT];
} g_snap;

static uint64_t mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u; }

void ss_request(int op) { atomic_store(&g_req, op); }
int ss_has_snapshot(void) { return g_snap.valid; }
int ss_feedback(float *age) { if (age) *age = (float)(mono_ms() - g_fb_time) / 1000.f; return g_fb_kind; }
static void feedback(int k) { g_fb_kind = k; g_fb_time = mono_ms(); }

void ssw(SSBuf *b, const void *src, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 4096; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, src, n);
    b->n += n;
}
int ssr(SSRd *r, void *dst, size_t n) {
    if ((size_t)(r->end - r->p) < n) { memset(dst, 0, n); r->p = r->end; return 0; }
    memcpy(dst, r->p, n);
    r->p += n;
    return 1;
}

/* Is r the return address of a CALL instruction in the game image? */
static int is_call_site(uint32_t r) {
    if (r < g_image_base + 0x1000u || r >= g_image_base + g_image_size) return 0;
    const uint8_t *p = gptr(r);
    if (p[-5] == 0xE8) return 1;
    for (int len = 2; len <= 7; len++) {          /* FF /2: call r/m32 */
        const uint8_t *q = p - len;
        if (q[0] != 0xFF || ((q[1] >> 3) & 7) != 2) continue;
        int mod = q[1] >> 6, rm = q[1] & 7, n = 2;
        if (mod != 3) {
            if (rm == 4) { n++; if (mod == 0 && (q[2] & 7) == 5) n += 4; }
            if (mod == 1) n += 1; else if (mod == 2) n += 4; else if (rm == 5) n += 4;
        }
        if (n == len) return 1;
    }
    return 0;
}
/* Return addresses (offset from esp, value) between esp and the top of the stack. */
int ss_chain(uint32_t esp, uint32_t top, uint32_t out[][2], int max) {
    int n = 0;
    for (uint32_t a = esp; a + 4 <= top && n < max; a += 4) {
        uint32_t v = g32(a);
        if (is_call_site(v)) { out[n][0] = a - esp; out[n][1] = v; n++; }
    }
    return n;
}
static int chain_of(uint32_t esp, uint32_t top, uint32_t out[][2]) { return ss_chain(esp, top, out, MAXCHAIN); }

/* Two call sites whose next instruction is a jmp to the same place continue identically. */
static uint32_t jmp_target(uint32_t a) {
    const uint8_t *p = gptr(a);
    if (p[0] == 0xEB) return a + 2 + (uint32_t)(int8_t)p[1];
    if (p[0] == 0xE9) { int32_t d; memcpy(&d, p + 1, 4); return a + 5 + (uint32_t)d; }
    return 0;
}
int ss_same_continuation(uint32_t a, uint32_t b) {
    if (a == b) return 1;
    uint32_t ta = jmp_target(a);
    return ta && ta == jmp_target(b);
}

static void ranges(uint32_t out[][2], int *n) {
    GHeapState st; gheap_get_state(&st);
    int k = 0;
    out[k][0] = g_image_base; out[k++][1] = g_image_size;
    out[k][0] = GUEST_STACK_TOP - GUEST_STACK_SIZE; out[k++][1] = GUEST_STACK_SIZE;
    out[k][0] = GUEST_HEAP_BASE; out[k++][1] = st.bump - GUEST_HEAP_BASE;
    out[k][0] = GUEST_VA_BASE; out[k++][1] = st.vbump - GUEST_VA_BASE;
    out[k][0] = GUEST_TEB; out[k++][1] = GUEST_SPACE - GUEST_TEB;
    *n = k;
}

static int take(CPU *c) {
    g_snap.valid = 0;
    g_snap.cpu = *c;
    g_snap.nchain = chain_of(c->esp, GUEST_STACK_TOP, g_snap.chain);
    gheap_get_state(&g_snap.heap);
    uint32_t rg[6][2]; int nr;
    ranges(rg, &nr);
    for (int i = 0; i < nr; i++) {
        Range *r = &g_snap.r[i];
        if (rg[i][1] > r->cap) { free(r->data); r->cap = rg[i][1]; r->data = malloc(r->cap); if (!r->data) { r->cap = 0; return 0; } }
        r->addr = rg[i][0]; r->len = rg[i][1];
        memcpy(r->data, gptr(r->addr), r->len);
    }
    g_snap.nr = nr;
    for (int i = 0; i < M_COUNT; i++) g_snap.mod[i].n = 0;
    k32_ss_save(&g_snap.mod[M_K32]);
    k32_time_save(&g_snap.mod[M_TIME]);
    files_ss_save(&g_snap.mod[M_FILES]);
    d3d_ss_save(&g_snap.mod[M_D3D]);
    ds_ss_save(&g_snap.mod[M_DS]);
    bink_ss_save(&g_snap.mod[M_BINK]);
    g_snap.valid = 1;
    return 1;
}

static SSRd rd(int m) { return (SSRd){g_snap.mod[m].p, g_snap.mod[m].p + g_snap.mod[m].n}; }

static int restore(CPU *c) {
    static uint32_t ch[MAXCHAIN][2];
    int n = chain_of(c->esp, GUEST_STACK_TOP, ch);
    if (c->esp != g_snap.cpu.esp || n != g_snap.nchain || memcmp(ch, g_snap.chain, sizeof ch[0] * (size_t)n)) {
        LOG("[ss] load refused: call chain differs (esp %08X/%08X, %d/%d return addresses)\n", c->esp, g_snap.cpu.esp, n, g_snap.nchain);
        return 0;
    }
    if (!k32_ss_check(rd(M_K32))) return 0;
    GHeapState now; gheap_get_state(&now);
    ds_ss_lock(1);
    for (int i = 0; i < g_snap.nr; i++) memcpy(gptr(g_snap.r[i].addr), g_snap.r[i].data, g_snap.r[i].len);
    /* memory handed out after the snapshot goes back to the never-used (zeroed) state */
    if (now.bump > g_snap.heap.bump) memset(gptr(g_snap.heap.bump), 0, now.bump - g_snap.heap.bump);
    if (now.vbump > g_snap.heap.vbump) memset(gptr(g_snap.heap.vbump), 0, now.vbump - g_snap.heap.vbump);
    gheap_set_state(&g_snap.heap);
    *c = g_snap.cpu;
    SSRd r;
    r = rd(M_TIME); k32_time_load(&r);
    r = rd(M_FILES); files_ss_load(&r);
    r = rd(M_D3D); d3d_ss_load(&r);
    r = rd(M_DS); ds_ss_load(&r);
    ds_ss_lock(0);
    r = rd(M_BINK); bink_ss_load(&r);
    r = rd(M_K32); k32_ss_load(&r);
    plat_ss_flush_input();
    return 1;
}

void ss_present(CPU *c) {
    int op = atomic_exchange(&g_req, SS_NONE);
    if (op == SS_NONE) return;
    if (op == SS_LOAD && !g_snap.valid) { LOG("[ss] nothing to load\n"); feedback(-1); return; }
    uint64_t t0 = mono_ms();
    if (!k32_park_others(1000)) { LOG("[ss] %s failed: a game thread did not stop\n", op == SS_SAVE ? "save" : "load"); feedback(-1); return; }
    int ok = op == SS_SAVE ? take(c) : restore(c);
    k32_unpark_others();
    size_t bytes = 0;
    for (int i = 0; i < g_snap.nr; i++) bytes += g_snap.r[i].len;
    LOG("[ss] %s %s in %llu ms (%zu MB)\n", op == SS_SAVE ? "save" : "load", ok ? "ok" : "FAILED",
        (unsigned long long)(mono_ms() - t0), bytes >> 20);
    feedback(ok ? (op == SS_SAVE ? 1 : 2) : -1);
}
