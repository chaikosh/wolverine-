/* Guest memory, loader, dispatcher, traps. */
#include "runtime.h"
#include <stdlib.h>
#include <stdarg.h>
#include <strings.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>

uint8_t *g_mem;
int g_trace_imports;
char g_game_dir[1024];

static GuestFn g_thunks[THUNK_MAX];
static const char *g_thunk_names[THUNK_MAX];
static uint32_t g_thunk_count;
static pthread_mutex_t g_thunk_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t rt_register_thunk(GuestFn fn, const char *name) {
    pthread_mutex_lock(&g_thunk_lock);
    uint32_t i = g_thunk_count++;
    pthread_mutex_unlock(&g_thunk_lock);
    if (i >= THUNK_MAX) { LOG("thunk table full\n"); abort(); }
    g_thunks[i] = fn;
    g_thunk_names[i] = name;
    return THUNK_BASE + i * THUNK_STRIDE;
}

const char *rt_thunk_name(uint32_t addr) {
    if (addr < THUNK_BASE) return NULL;
    uint32_t i = (addr - THUNK_BASE) / THUNK_STRIDE;
    return i < g_thunk_count ? g_thunk_names[i] : NULL;
}

static GuestFn find_func(uint32_t addr) {
    uint32_t lo = 0, hi = g_func_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_func_table[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return (lo < g_func_count && g_func_table[lo].addr == addr) ? g_func_table[lo].fn : NULL;
}

const char *rt_func_name(uint32_t addr) {
    static __thread char buf[64];
    const char *t = rt_thunk_name(addr);
    if (t) return t;
    snprintf(buf, sizeof buf, "sub_%08X%s", addr, find_func(addr) ? "" : "?");
    return buf;
}

__thread uint32_t g_cur_thunk;

void dispatch(CPU *c, uint32_t target) {
    if (target >= THUNK_BASE) {
        uint32_t i = (target - THUNK_BASE) / THUNK_STRIDE;
        if (i < g_thunk_count && (target - THUNK_BASE) % THUNK_STRIDE == 0) {
            uint32_t prev = g_cur_thunk;
            g_cur_thunk = target;
            if (g_trace_imports > 1) LOG("[call] %s\n", g_thunk_names[i]);
            g_thunks[i](c);
            g_cur_thunk = prev;
            return;
        }
    } else {
        GuestFn fn = find_func(target);
        if (fn) { fn(c); return; }
    }
    rt_trap(c, target, "dispatch to unknown address");
}

void rt_backtrace(CPU *c) {
    LOG("  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
        c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    LOG("  guest stack (code-looking return addresses):\n");
    int n = 0;
    for (uint32_t p = c->esp; p < GUEST_STACK_TOP && n < 16; p += 4) {
        uint32_t v = g32(p);
        if (v >= g_image_base + 0x1000 && v < g_image_base + 0x1D4000) {
            LOG("    [%08X] %08X\n", p, v);
            n++;
        }
    }
}

void rt_trap(CPU *c, uint32_t addr, const char *what) {
    LOG("\n*** TRAP at %08X: %s\n", addr, what);
    rt_backtrace(c);
    fflush(stderr);
    exit(2);
}

void rt_unimpl_import(CPU *c, int idx) {
    LOG("\n*** UNIMPLEMENTED IMPORT %s!%s (called from %08X)\n",
        g_import_desc[idx].dll, g_import_desc[idx].name, g32(c->esp));
    rt_backtrace(c);
    fflush(stderr);
    exit(3);
}

void rt_cpuid(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    /* Present a plain P6-class CPU without MMX/SSE/3DNow!, steering D3DX math onto x87 paths. */
    uint32_t leaf = *a;
    if (leaf == 0) { *a = 1; *b = 0x756E6547; *d = 0x49656E69; *c = 0x6C65746E; }   /* GenuineIntel */
    else if (leaf == 1) { *a = 0x00000683; *b = 0; *c = 0; *d = 0x00008111; }       /* FPU TSC CX8 CMOV */
    else if (leaf == 0x80000000u) { *a = 0x80000000u; *b = *c = *d = 0; }
    else { *a = *b = *c = *d = 0; }
}

uint64_t rt_rdtsc(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;   /* ~1 GHz "TSC" */
}

/* Host -> guest call: pushes args right-to-left and a sentinel return address. */
uint32_t rt_call_guest(CPU *c, uint32_t fn, int nargs, ...) {
    va_list ap;
    uint32_t args[16];
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t saved_esp = c->esp;
    for (int i = nargs - 1; i >= 0; i--) { c->esp -= 4; s32(c->esp, args[i]); }
    c->esp -= 4; s32(c->esp, 0xFFFFFFF0u);   /* sentinel return address */
    dispatch(c, fn);
    c->esp = saved_esp;                       /* works for both stdcall and cdecl callees */
    return c->eax;
}

static void sanitize(char *dst, const char *src) {
    for (; *src; src++) *dst++ = ((*src >= 'a' && *src <= 'z') || (*src >= 'A' && *src <= 'Z') ||
                                  (*src >= '0' && *src <= '9')) ? *src : '_';
    *dst = 0;
}

static void load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { LOG("cannot open %s\n", path); exit(1); }
    size_t n = fread(g_mem + g_image_base, 1, g_image_size, f);
    fclose(f);
    if (n != g_image_size) { LOG("image.bin size mismatch (%zu != %u)\n", n, g_image_size); exit(1); }
}

static void bind_imports(void) {
    int bound = 0;
    for (uint32_t i = 0; i < g_import_count; i++) {
        char dll[64], name[128], sym[256];
        sanitize(dll, g_import_desc[i].dll);
        char *dot = strstr(dll, "_DLL"); if (dot) *dot = 0;
        sanitize(name, g_import_desc[i].name);
        snprintf(sym, sizeof sym, "shim_%s_%s", dll, name);
        GuestFn fn = (GuestFn)dlsym(RTLD_DEFAULT, sym);
        if (fn) bound++; else fn = g_import_stubs[i];
        g_imports[i] = fn;
        char *label = malloc(strlen(g_import_desc[i].dll) + strlen(g_import_desc[i].name) + 2);
        sprintf(label, "%s!%s", g_import_desc[i].dll, g_import_desc[i].name);
        s32(g_import_desc[i].iat, rt_register_thunk(fn, label));
    }
    LOG("[rt] imports: %d/%u implemented\n", bound, g_import_count);
}

void win32_init(CPU *c);

/* Xbox-style gamepad layout written into [Controller] once:
   A jump/confirm, B action/strike, X punch, Y kick, LB stealth, RB claws, LT crouch, RT senses, RS camera reset. */
static void apply_pad_layout(const char *ini) {
    static const struct { const char *k; int v; } L[] = {{"Punch", 2}, {"Kick", 3}, {"Jump", 0}, {"Action", 1}, {"Claws", 5},
        {"Crouch", 10}, {"Stealth", 4}, {"Senses", 11}, {"Camera", 9}, {"Type", 5}, {"CAMLR", 1}, {"CAMUD", 2}};
    FILE *f = fopen(ini, "rb");
    if (!f) return;
    char buf[16384]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    if (strstr(buf, "; xbox-layout")) return;
    char out[20000]; size_t o = 0; int in = 0;
    for (char *save, *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (line[0] == '[') in = !strncasecmp(line, "[Controller]", 12);
        int done = 0;
        if (in)
            for (unsigned i = 0; i < sizeof L / sizeof L[0]; i++) {
                size_t kl = strlen(L[i].k);
                if (!strncasecmp(line, L[i].k, kl) && line[kl] == '=') { o += (size_t)snprintf(out + o, sizeof out - o, "%s=%d\r\n", L[i].k, L[i].v); done = 1; break; }
            }
        if (!done) o += (size_t)snprintf(out + o, sizeof out - o, "%s\r\n", line);
    }
    o += (size_t)snprintf(out + o, sizeof out - o, "; xbox-layout\r\n");
    if ((f = fopen(ini, "wb"))) { fwrite(out, 1, o, f); fclose(f); }
}

static void fault_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uintptr_t a = (uintptr_t)si->si_addr, base = (uintptr_t)g_mem;
    char buf[256];
    int n;
    if (a >= base && a < base + GUEST_SPACE + 0x10000)
        n = snprintf(buf, sizeof buf, "\n*** HOST %s: guest address %08lX (thunk %s)\n", sig == SIGBUS ? "SIGBUS" : "SIGSEGV",
                     (unsigned long)(a - base), g_cur_thunk ? rt_thunk_name(g_cur_thunk) : "-");
    else
        n = snprintf(buf, sizeof buf, "\n*** HOST %s: host address %p\n", sig == SIGBUS ? "SIGBUS" : "SIGSEGV", si->si_addr);
    write(2, buf, (size_t)n);
    void *bt[48];
    int k = backtrace(bt, 48);
    backtrace_symbols_fd(bt, k, 2);
    _exit(6);
}

static void exit_sentinel(CPU *c) { LOG("[rt] entry point returned eax=%08X\n", c->eax); exit((int)c->eax); }

extern char g_write_dir[1024];

int main(int argc, char **argv) {
    char image_buf[1024];
    const char *image;
    if (argc > 2) {   /* desktop: wolvie <image.bin> <game dir> */
        image = argv[1];
        snprintf(g_game_dir, sizeof g_game_dir, "%s", argv[2]);
        snprintf(g_write_dir, sizeof g_write_dir, "%s", argv[2]);
    } else {          /* app bundle: image.bin + game/ inside resources, writes go to Documents */
        const char *base = SDL_GetBasePath();
        snprintf(image_buf, sizeof image_buf, "%simage.bin", base ? base : "./");
        image = image_buf;
        snprintf(g_game_dir, sizeof g_game_dir, "%sgame", base ? base : "./");
        const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
        snprintf(g_write_dir, sizeof g_write_dir, "%s", docs ? docs : g_game_dir);
        size_t l = strlen(g_write_dir);
        if (l && g_write_dir[l - 1] == '/') g_write_dir[l - 1] = 0;
        mkdir(g_write_dir, 0755);
        /* device builds: keep a log next to the saves (Documents/wolverine.log, previous run in .prev) */
        char logp[1200], prevp[1200];
        snprintf(logp, sizeof logp, "%s/wolverine.log", g_write_dir);
        snprintf(prevp, sizeof prevp, "%s/wolverine.prev.log", g_write_dir);
        rename(logp, prevp);
        freopen(logp, "w", stderr);
        setvbuf(stderr, NULL, _IONBF, 0);
        setenv("TRACE_FRAMES", "1", 0);
        char ini_w[1200], ini_r[1200];
        snprintf(ini_w, sizeof ini_w, "%s/Wolvie.INI", g_write_dir);
        snprintf(ini_r, sizeof ini_r, "%s/Wolvie.INI", g_game_dir);
        struct stat st;
        if (stat(ini_w, &st) != 0) {   /* first launch: seed the editable settings file */
            FILE *a = fopen(ini_r, "rb"), *b = fopen(ini_w, "wb");
            if (a && b) { char buf[8192]; size_t k; while ((k = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, k, b); }
            if (a) fclose(a);
            if (b) fclose(b);
        }
    }
    g_trace_imports = getenv("TRACE_IMPORTS") ? atoi(getenv("TRACE_IMPORTS")) : 0;
    g_mem = mmap(NULL, (size_t)GUEST_SPACE + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED) { perror("mmap guest space"); return 1; }
    mprotect(g_mem, 0x10000, PROT_NONE);                       /* catch guest NULL derefs */
    struct sigaction sa = {0};
    sa.sa_sigaction = fault_handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    stack_t ss = {.ss_sp = malloc(1 << 16), .ss_size = 1 << 16};
    sigaltstack(&ss, NULL);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    load_image(image);
    gheap_init();
    bind_imports();
    CPU *c = calloc(1, sizeof(CPU));
    {
        char ini[1200];
        snprintf(ini, sizeof ini, "%s/Wolvie.INI", g_write_dir);
        apply_pad_layout(ini);
    }
    if (argc <= 2) {   /* app bundle: match the screen aspect on first launch */
        char ini[1200];
        snprintf(ini, sizeof ini, "%s/Wolvie.INI", g_write_dir);
        FILE *f = fopen(ini, "rb");
        char buf[8192] = {0};
        size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        if (f) fclose(f);
        if (n && !strstr(buf, "; ios-res")) {
            extern void plat_init(void);
            extern void plat_native_mode(int *w, int *h);
            plat_init();
            int w, h;
            plat_native_mode(&w, &h);
            if (w > 0 && h > 0) {
                int mh = 900, mw = ((int)((double)w * mh / h) + 7) & ~7;
                char out[9000]; size_t o = 0;
                for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
                    if (!strncmp(line, "ScreenWidth=", 12)) o += (size_t)snprintf(out + o, sizeof out - o, "ScreenWidth=%d\r\n", mw);
                    else if (!strncmp(line, "ScreenHeight=", 13)) o += (size_t)snprintf(out + o, sizeof out - o, "ScreenHeight=%d\r\n", mh);
                    else o += (size_t)snprintf(out + o, sizeof out - o, "%s\n", line);
                }
                o += (size_t)snprintf(out + o, sizeof out - o, "; ios-res\r\n");
                f = fopen(ini, "wb");
                if (f) { fwrite(out, 1, o, f); fclose(f); }
                LOG("[rt] screen %dx%d -> game resolution %dx%d\n", w, h, mw, mh);
            }
        }
    }
    c->fpu_cw = 0x027F;
    c->fs_base = GUEST_TEB;
    c->esp = GUEST_STACK_TOP - 0x100;
    win32_init(c);
    uint32_t sentinel = rt_register_thunk(exit_sentinel, "<exit>");
    c->esp -= 4; s32(c->esp, sentinel);
    LOG("[rt] %u functions, entry %08X, game dir %s, write dir %s\n", g_func_count, g_entry_point, g_game_dir, g_write_dir);
    dispatch(c, g_entry_point);
    exit_sentinel(c);
}

/* ---- setjmp/longjmp: guest jmp_buf (MSVC layout) paired with a host jmp_buf ---- */
typedef struct { uint32_t guest_buf, esp_after; jmp_buf hb; } SJSlot;
static __thread SJSlot g_sj[32];
static __thread int g_sj_next;

jmp_buf *rt_setjmp_slot(CPU *c, uint32_t guest_buf, uint32_t esp_after) {
    (void)c;
    for (int i = 0; i < 32; i++)
        if (g_sj[i].guest_buf == guest_buf) { g_sj[i].esp_after = esp_after; return &g_sj[i].hb; }
    SJSlot *s = &g_sj[g_sj_next++ & 31];
    s->guest_buf = guest_buf; s->esp_after = esp_after;
    return &s->hb;
}

void rt_longjmp(CPU *c) {
    uint32_t buf = ARG(c, 0), val = ARG(c, 1);
    for (int i = 0; i < 32; i++) {
        if (g_sj[i].guest_buf != buf) continue;
        c->ebp = g32(buf); c->ebx = g32(buf + 4); c->edi = g32(buf + 8); c->esi = g32(buf + 12);
        c->esp = g_sj[i].esp_after;
        c->eax = val ? val : 1;
        s32(c->fs_base, g32(buf + 0x18));   /* restore SEH chain head */
        _longjmp(g_sj[i].hb, 1);
    }
    rt_trap(c, g32(c->esp), "longjmp to unknown jmp_buf");
}
