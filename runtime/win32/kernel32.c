/* KERNEL32 shims: process, memory, strings/locale, files, INI, time, sync. */
#include "../runtime.h"
#include "win32.h"
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include "../savestate.h"

#define S(name) void shim_KERNEL32_##name(CPU *c)

static uint32_t g_cmdline, g_envA, g_envW;
static uint32_t g_tls_next;

void plat_init(void);

void win32_init(CPU *c) {
    plat_init();
    /* TEB / PEB */
    memset(gptr(GUEST_TEB), 0, 0x1000);
    s32(GUEST_TEB + 0x00, 0xFFFFFFFFu);                 /* SEH chain end */
    s32(GUEST_TEB + 0x04, GUEST_STACK_TOP);
    s32(GUEST_TEB + 0x08, GUEST_STACK_TOP - GUEST_STACK_SIZE);
    s32(GUEST_TEB + 0x18, GUEST_TEB);
    s32(GUEST_TEB + 0x20, 0x1000);                      /* pid */
    s32(GUEST_TEB + 0x24, 0x1004);                      /* tid */
    s32(GUEST_TEB + 0x30, GUEST_PEB);
    s32(GUEST_PEB + 0x08, g_image_base);
    c->thread_id = 0x1004;
    g_cmdline = gstrdup("\"" VIRT_DIR "\\Wolvie.exe\"");
    static const char envA[] = "WINDIR=C:\\WINDOWS\0\0";
    g_envA = galloc(sizeof envA, 0);
    memcpy(gptr(g_envA), envA, sizeof envA);
    g_envW = galloc(sizeof envA * 2, 0);
    for (size_t i = 0; i < sizeof envA; i++) s16(g_envW + 2 * (uint32_t)i, (uint8_t)envA[i]);
}

/* ------------------------------------------------------------ process / module */
S(GetVersion) { RET(c, 0x0A280105u, 0); }                      /* Windows XP SP3 */
S(GetVersionExA) {
    uint32_t p = ARG(c, 0);
    s32(p + 4, 5); s32(p + 8, 1); s32(p + 12, 2600); s32(p + 16, 2);
    strcpy((char *)gptr(p + 20), "Service Pack 3");
    RET(c, 1, 1);
}
S(GetCommandLineA) { RET(c, g_cmdline, 0); }
S(GetEnvironmentStrings) { RET(c, g_envA, 0); }
S(GetEnvironmentStringsW) { RET(c, g_envW, 0); }
S(FreeEnvironmentStringsA) { RET(c, 1, 1); }
S(FreeEnvironmentStringsW) { RET(c, 1, 1); }
S(GetEnvironmentVariableA) { RET(c, 0, 3); }
S(SetEnvironmentVariableA) { RET(c, 1, 2); }
S(GetStartupInfoA) { uint32_t p = ARG(c, 0); memset(gptr(p), 0, 68); s32(p, 68); RET(c, 0, 1); }
S(GetModuleFileNameA) {
    const char *path = VIRT_DIR "\\Wolvie.exe";
    uint32_t buf = ARG(c, 1), n = ARG(c, 2), len = (uint32_t)strlen(path);
    if (len >= n) len = n - 1;
    memcpy(gptr(buf), path, len); gptr(buf)[len] = 0;
    RET(c, len, 3);
}
S(GetModuleHandleA) {
    const char *name = gstr(ARG(c, 0));
    RET(c, name ? module_handle(name) : g_image_base, 1);
}
S(LoadLibraryA) { RET(c, module_handle(gstr(ARG(c, 0))), 1); }
S(GetProcAddress) {
    uint32_t h = ARG(c, 0), name = ARG(c, 1);
    char sym[256];
    if (name < 0x10000) snprintf(sym, sizeof sym, "ord%u", name);
    else snprintf(sym, sizeof sym, "%s", gstr(name));
    RET(c, resolve_export(module_name(h), sym), 2);
}
S(ExitProcess) { LOG("[k32] ExitProcess(%u)\n", ARG(c, 0)); exit((int)ARG(c, 0)); }
S(TerminateProcess) { LOG("[k32] TerminateProcess(%u)\n", ARG(c, 1)); exit((int)ARG(c, 1)); }
S(FatalAppExitA) { LOG("[k32] FatalAppExit: %s\n", gstr(ARG(c, 1))); exit(4); }
S(GetCurrentProcess) { RET(c, 0xFFFFFFFFu, 0); }
S(GetCurrentThread) { RET(c, 0xFFFFFFFEu, 0); }
S(GetCurrentThreadId) { RET(c, c->thread_id, 0); }
S(SetUnhandledExceptionFilter) { RET(c, 0, 1); }
S(UnhandledExceptionFilter) { LOG("[k32] UnhandledExceptionFilter\n"); RET(c, 1, 1); }
S(IsBadReadPtr) { RET(c, ARG(c, 0) < 0x10000, 2); }
S(IsBadWritePtr) { RET(c, ARG(c, 0) < 0x10000, 2); }
S(IsBadCodePtr) { RET(c, ARG(c, 0) < 0x10000, 1); }
S(IsProcessorFeaturePresent) { RET(c, 0, 1); }
char g_cur_load[256];   /* last allLoad("...") the game announced; used to tag HUD textures */
S(OutputDebugStringA) {
    const char *m = gstr(ARG(c, 0));
    LOG("[dbg] %s", m);
    const char *p = m ? strstr(m, "allLoad(\"") : NULL;
    if (p) { snprintf(g_cur_load, sizeof g_cur_load, "%s", p + 9); char *q = strchr(g_cur_load, '"'); if (q) *q = 0; }
    RET(c, 0, 1);
}
S(GetLastError) { RET(c, g32(c->fs_base + 0x34), 0); }
S(SetLastError) { s32(c->fs_base + 0x34, ARG(c, 0)); RET(c, 0, 1); }
S(SetConsoleCtrlHandler) { RET(c, 1, 2); }
S(SetStdHandle) { RET(c, 1, 2); }
S(GetStdHandle) { RET(c, 0x10000u + (uint32_t)(-(int32_t)ARG(c, 0)), 1); }
S(SetHandleCount) { RET(c, ARG(c, 0), 1); }
S(RaiseException) {
    LOG("[k32] RaiseException code=%08X flags=%u nargs=%u\n", ARG(c, 0), ARG(c, 1), ARG(c, 2));
    rt_trap(c, g32(c->esp), "RaiseException (C++ throw?) not supported yet");
}
S(RtlUnwind) { rt_trap(c, g32(c->esp), "RtlUnwind not supported yet"); }

/* ------------------------------------------------------------ memory */
S(HeapCreate) { RET(c, 0x00150000u, 3); }
S(HeapDestroy) { RET(c, 1, 1); }
S(HeapAlloc) { RET(c, galloc(ARG(c, 2), (ARG(c, 1) & 8) != 0), 3); }
S(HeapFree) { gfree(ARG(c, 2)); RET(c, 1, 3); }
S(HeapSize) { RET(c, gsize(ARG(c, 2)), 3); }
S(HeapReAlloc) {
    uint32_t flags = ARG(c, 1), p = ARG(c, 2), n = ARG(c, 3), old = gsize(p);
    uint32_t r;
    if (flags & 0x10) r = (p && n <= g32(p - 4)) ? (s32(p - 16, n), p) : 0;   /* in-place only */
    else r = grealloc(p, n);
    if (r && (flags & 8) && n > old) memset(gptr(r + old), 0, n - old);
    RET(c, r, 4);
}
S(VirtualAlloc) {
    uint32_t addr = ARG(c, 0), size = ARG(c, 1);
    if (addr) RET(c, addr & ~0xFFFu, 4);            /* commit inside an earlier reservation */
    else RET(c, gvalloc(size), 4);
}
S(VirtualFree) {
    uint32_t addr = ARG(c, 0), size = ARG(c, 1), type = ARG(c, 2);
    if ((type & 0x4000) && addr && size) memset(gptr(addr), 0, size);   /* MEM_DECOMMIT */
    RET(c, 1, 3);
}

/* ------------------------------------------------------------ TLS / sync */
S(TlsAlloc) { RET(c, g_tls_next < 64 ? g_tls_next++ : 0xFFFFFFFFu, 0); }
S(TlsFree) { RET(c, 1, 1); }
S(TlsGetValue) { s32(c->fs_base + 0x34, 0); RET(c, g32(c->fs_base + 0xE10 + 4 * ARG(c, 0)), 1); }
S(TlsSetValue) { s32(c->fs_base + 0xE10 + 4 * ARG(c, 0), ARG(c, 1)); RET(c, 1, 2); }
S(InitializeCriticalSection) { cs_get(ARG(c, 0)); RET(c, 0, 1); }
S(DeleteCriticalSection) { RET(c, 0, 1); }
S(EnterCriticalSection) { pthread_mutex_lock(cs_get(ARG(c, 0))); RET(c, 0, 1); }
S(LeaveCriticalSection) { pthread_mutex_unlock(cs_get(ARG(c, 0))); RET(c, 0, 1); }
S(InterlockedIncrement) {
    _Atomic uint32_t *p = (_Atomic uint32_t *)gptr(ARG(c, 0));
    RET(c, atomic_fetch_add(p, 1) + 1, 1);
}
S(InterlockedDecrement) {
    _Atomic uint32_t *p = (_Atomic uint32_t *)gptr(ARG(c, 0));
    RET(c, atomic_fetch_sub(p, 1) - 1, 1);
}

/* ------------------------------------------------------------ time */
static uint64_t now_us(void) { struct timeval tv; gettimeofday(&tv, NULL); return (uint64_t)tv.tv_sec * 1000000u + tv.tv_usec; }
static _Atomic int64_t g_time_off;   /* save states: guest clocks continue from the saved moment */
static uint64_t mono_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u + (uint64_t)atomic_load(&g_time_off);
}
void k32_time_save(SSBuf *b) { uint64_t t = mono_us(); SSW(b, t); }
void k32_time_load(SSRd *r) { uint64_t t; if (SSR(r, t)) atomic_fetch_add(&g_time_off, (int64_t)(t - mono_us())); }
S(GetTickCount) { RET(c, (uint32_t)(mono_us() / 1000), 0); }
S(QueryPerformanceFrequency) { uint32_t p = ARG(c, 0); s32(p, 1000000); s32(p + 4, 0); RET(c, 1, 1); }
S(QueryPerformanceCounter) { uint32_t p = ARG(c, 0); uint64_t t = mono_us(); s32(p, (uint32_t)t); s32(p + 4, (uint32_t)(t >> 32)); RET(c, 1, 1); }
static atomic_int g_park_req;
static void park_point(CPU *c);
S(Sleep) {
    if (atomic_load(&g_park_req)) park_point(c);   /* a save/load is waiting for this thread */
    usleep(ARG(c, 0) * 1000u);
    RET(c, 0, 1);
}
static void put_systime(uint32_t p, int local) {
    time_t t = time(NULL); struct tm tm;
    if (local) localtime_r(&t, &tm); else gmtime_r(&t, &tm);
    uint16_t v[8] = {(uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_wday, (uint16_t)tm.tm_mday,
                     (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min, (uint16_t)tm.tm_sec, (uint16_t)((now_us() / 1000) % 1000)};
    memcpy(gptr(p), v, 16);
}
S(GetLocalTime) { put_systime(ARG(c, 0), 1); RET(c, 0, 1); }
S(GetSystemTime) { put_systime(ARG(c, 0), 0); RET(c, 0, 1); }
S(GetTimeZoneInformation) { memset(gptr(ARG(c, 0)), 0, 172); RET(c, 0, 1); }
S(FileTimeToLocalFileTime) { memcpy(gptr(ARG(c, 1)), gptr(ARG(c, 0)), 8); RET(c, 1, 2); }
S(FileTimeToSystemTime) {
    uint64_t ft; memcpy(&ft, gptr(ARG(c, 0)), 8);
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull); struct tm tm; gmtime_r(&t, &tm);
    uint16_t v[8] = {(uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_wday, (uint16_t)tm.tm_mday,
                     (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min, (uint16_t)tm.tm_sec, 0};
    memcpy(gptr(ARG(c, 1)), v, 16);
    RET(c, 1, 2);
}

/* ------------------------------------------------------------ code pages / locale */
S(GetACP) { RET(c, 1252, 0); }
S(GetOEMCP) { RET(c, 437, 0); }
S(IsValidCodePage) { RET(c, 1, 1); }
S(GetCPInfo) { uint32_t p = ARG(c, 1); memset(gptr(p), 0, 20); s32(p, 1); gptr(p)[4] = '?'; RET(c, 1, 2); }
S(GetUserDefaultLCID) { RET(c, 0x0409, 0); }
S(IsValidLocale) { RET(c, 1, 2); }
S(EnumSystemLocalesA) { RET(c, 1, 2); }
S(GetLocaleInfoA) { RET(c, 0, 4); }
S(GetLocaleInfoW) { RET(c, 0, 4); }
S(MultiByteToWideChar) {
    uint32_t src = ARG(c, 2), dst = ARG(c, 4); int32_t n = (int32_t)ARG(c, 3), cap = (int32_t)ARG(c, 5);
    if (n < 0) n = (int32_t)strlen(gstr(src)) + 1;
    if (!cap) RET(c, (uint32_t)n, 6);
    else {
        int32_t m = n < cap ? n : cap;
        for (int32_t i = 0; i < m; i++) s16(dst + 2 * (uint32_t)i, gptr(src)[i]);
        RET(c, (uint32_t)m, 6);
    }
}
S(WideCharToMultiByte) {
    uint32_t src = ARG(c, 2), dst = ARG(c, 4); int32_t n = (int32_t)ARG(c, 3), cap = (int32_t)ARG(c, 5);
    if (n < 0) { n = 0; while (g16(src + 2 * (uint32_t)n)) n++; n++; }
    if (!cap) RET(c, (uint32_t)n, 8);
    else {
        int32_t m = n < cap ? n : cap;
        for (int32_t i = 0; i < m; i++) { uint16_t w = g16(src + 2 * (uint32_t)i); gptr(dst)[i] = w < 256 ? (uint8_t)w : '?'; }
        RET(c, (uint32_t)m, 8);
    }
}
static uint16_t ctype1(unsigned ch) {
    uint16_t t = 0;
    if (ch < 128) {
        if (isupper(ch)) t |= 0x001; if (islower(ch)) t |= 0x002; if (isdigit(ch)) t |= 0x004;
        if (isspace(ch)) t |= 0x008; if (ispunct(ch)) t |= 0x010; if (iscntrl(ch)) t |= 0x020;
        if (ch == ' ' || ch == '\t') t |= 0x040; if (isxdigit(ch)) t |= 0x080; if (isalpha(ch)) t |= 0x100;
    } else if (ch >= 0xC0 && ch != 0xD7 && ch != 0xF7) t = 0x100 | (ch < 0xE0 ? 0x001 : 0x002);
    return t;
}
S(GetStringTypeA) {
    uint32_t src = ARG(c, 2), out = ARG(c, 4); int32_t n = (int32_t)ARG(c, 3);
    if (n < 0) n = (int32_t)strlen(gstr(src)) + 1;
    for (int32_t i = 0; i < n; i++) s16(out + 2 * (uint32_t)i, ctype1(gptr(src)[i]));
    RET(c, 1, 5);
}
S(GetStringTypeW) {
    uint32_t src = ARG(c, 1), out = ARG(c, 3); int32_t n = (int32_t)ARG(c, 2);
    if (n < 0) { n = 0; while (g16(src + 2 * (uint32_t)n)) n++; n++; }
    for (int32_t i = 0; i < n; i++) s16(out + 2 * (uint32_t)i, ctype1(g16(src + 2 * (uint32_t)i)));
    RET(c, 1, 4);
}
static void lcmap(CPU *c, int wide) {
    uint32_t flags = ARG(c, 1), src = ARG(c, 2), dst = ARG(c, 4); int32_t n = (int32_t)ARG(c, 3), cap = (int32_t)ARG(c, 5);
    int w = wide ? 2 : 1;
    if (n < 0) { n = 0; while (wide ? g16(src + 2 * (uint32_t)n) : gptr(src)[n]) n++; n++; }
    if (!cap) { RET(c, (uint32_t)n, 6); return; }
    int32_t m = n < cap ? n : cap;
    for (int32_t i = 0; i < m; i++) {
        unsigned ch = wide ? g16(src + 2 * (uint32_t)i) : gptr(src)[i];
        if (flags & 0x200) ch = (ch < 128) ? (unsigned)toupper((int)ch) : ch;
        if (flags & 0x100) ch = (ch < 128) ? (unsigned)tolower((int)ch) : ch;
        if (wide) s16(dst + 2 * (uint32_t)i, (uint16_t)ch); else gptr(dst)[i] = (uint8_t)ch;
    }
    (void)w;
    RET(c, (uint32_t)m, 6);
}
S(LCMapStringA) { lcmap(c, 0); }
S(LCMapStringW) { lcmap(c, 1); }
static void cmpstr(CPU *c, int wide) {
    uint32_t flags = ARG(c, 1), a = ARG(c, 2), b = ARG(c, 4); int32_t na = (int32_t)ARG(c, 3), nb = (int32_t)ARG(c, 5);
#define CH(p, i) (wide ? g16((p) + 2 * (uint32_t)(i)) : gptr(p)[i])
    if (na < 0) { na = 0; while (CH(a, na)) na++; }
    if (nb < 0) { nb = 0; while (CH(b, nb)) nb++; }
    int32_t i = 0; int r = 0;
    for (; i < na && i < nb; i++) {
        unsigned x = CH(a, i), y = CH(b, i);
        if (flags & 1) { x = x < 128 ? (unsigned)tolower((int)x) : x; y = y < 128 ? (unsigned)tolower((int)y) : y; }
        if (x != y) { r = x < y ? -1 : 1; break; }
    }
    if (!r && na != nb) r = na < nb ? -1 : 1;
#undef CH
    RET(c, (uint32_t)(r + 2), 6);
}
S(CompareStringA) { cmpstr(c, 0); }
S(CompareStringW) { cmpstr(c, 1); }

/* ------------------------------------------------------------ console (unused by the game) */
S(GetConsoleMode) { RET(c, 0, 2); }
S(SetConsoleMode) { RET(c, 0, 2); }
S(WriteConsoleA) { RET(c, 0, 5); }
S(GetNumberOfConsoleInputEvents) { RET(c, 0, 2); }
S(PeekConsoleInputA) { RET(c, 0, 4); }
S(ReadConsoleInputA) { RET(c, 0, 4); }
S(FlushConsoleInputBuffer) { RET(c, 0, 1); }
S(GetConsoleScreenBufferInfo) { RET(c, 0, 2); }

/* ------------------------------------------------------------ threads */
typedef struct GThread {
    CPU cpu;
    uint32_t start, param, handle;
    pthread_t pt;
    pthread_mutex_t m; pthread_cond_t cv;
    int slot, suspended, finished;
    uint32_t exit_code;
    int parked, zombie;          /* save states: stopped at a Sleep call / to be retired on wake-up */
    uint32_t park_ret;           /* return address of that Sleep call */
} GThread;
static GThread *g_threads[64];
static pthread_mutex_t g_thr_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_next_tid = 0x2000;
static __thread GThread *tl_self;

static void thread_cleanup(void *arg) {   /* runs on return and on ExitThread: free the slot for new threads */
    GThread *t = arg;
    pthread_mutex_lock(&g_thr_lock);
    if (g_threads[t->slot] == t) g_threads[t->slot] = NULL;
    pthread_mutex_unlock(&g_thr_lock);
    pthread_detach(pthread_self());
    free(t);
}
static void *thread_main(void *arg) {
    GThread *t = arg;
    tl_self = t;
    pthread_cleanup_push(thread_cleanup, t);
    pthread_mutex_lock(&t->m);
    while (t->suspended) pthread_cond_wait(&t->cv, &t->m);
    pthread_mutex_unlock(&t->m);
    if (!t->zombie) t->exit_code = rt_call_guest(&t->cpu, t->start, 1, t->param);
    t->finished = 1;
    pthread_cleanup_pop(1);
    return NULL;
}
static GThread *thread_of(uint32_t h) {
    uint32_t i = h - 0x3000u;
    return (h >= 0x3000u && i < 64) ? g_threads[i] : NULL;
}

/* ---- save states: every other guest thread stops at its next Sleep call and waits there */
static pthread_mutex_t g_park_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_park_cv = PTHREAD_COND_INITIALIZER;
static void park_point(CPU *c) {
    GThread *t = tl_self;
    if (!t) return;
    pthread_mutex_lock(&g_park_m);
    t->park_ret = g32(c->esp);
    t->parked = 1;
    pthread_cond_broadcast(&g_park_cv);
    while (atomic_load(&g_park_req)) pthread_cond_wait(&g_park_cv, &g_park_m);
    t->parked = 0;
    int z = t->zombie;
    pthread_mutex_unlock(&g_park_m);
    if (z) pthread_exit(NULL);   /* not part of the loaded state */
}
int k32_park_others(int timeout_ms) {
    atomic_store(&g_park_req, 1);
    struct timespec end; clock_gettime(CLOCK_REALTIME, &end);
    end.tv_sec += timeout_ms / 1000; end.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (end.tv_nsec >= 1000000000L) { end.tv_sec++; end.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_park_m);
    for (;;) {
        int pending = 0;
        pthread_mutex_lock(&g_thr_lock);
        for (int i = 0; i < 64; i++) {
            GThread *t = g_threads[i];
            if (t && t != tl_self && !t->finished && !t->suspended && !t->parked) pending++;
        }
        pthread_mutex_unlock(&g_thr_lock);
        if (!pending) break;
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > end.tv_sec || (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec)) {
            atomic_store(&g_park_req, 0);
            pthread_cond_broadcast(&g_park_cv);
            pthread_mutex_unlock(&g_park_m);
            return 0;
        }
        struct timespec w = now; w.tv_nsec += 5000000L;
        if (w.tv_nsec >= 1000000000L) { w.tv_sec++; w.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_park_cv, &g_park_m, &w);
    }
    pthread_mutex_unlock(&g_park_m);
    return 1;
}
void k32_unpark_others(void) {
    pthread_mutex_lock(&g_park_m);
    atomic_store(&g_park_req, 0);
    pthread_cond_broadcast(&g_park_cv);
    pthread_mutex_unlock(&g_park_m);
}

#define TCHAIN 64
typedef struct { uint32_t start, ret, depth; CPU cpu; int nchain; uint32_t chain[TCHAIN][2]; } SSThread;
static int parked_threads(GThread **out) {
    int n = 0;
    for (int i = 0; i < 64; i++) if (g_threads[i] && g_threads[i]->parked && !g_threads[i]->zombie) out[n++] = g_threads[i];
    return n;
}
static void describe(GThread *t, SSThread *s) {
    memset(s, 0, sizeof *s);
    s->start = t->start; s->ret = t->park_ret; s->cpu = t->cpu;
    s->depth = t->cpu.fs_base - t->cpu.esp;            /* the stack ends at the thread's TEB */
    s->nchain = ss_chain(t->cpu.esp, t->cpu.fs_base, s->chain, TCHAIN);
}
void k32_ss_save(SSBuf *b) {
    GThread *live[64]; int n = parked_threads(live);
    SSW(b, n);
    for (int i = 0; i < n; i++) { SSThread s; describe(live[i], &s); SSW(b, s); }
}
/* A saved thread can continue in a live one that runs the same code, is stopped at an equivalent Sleep
   call and has the same call chain above it (the stack itself comes back with guest memory). */
static int compatible(const SSThread *s, const SSThread *l) {
    if (s->start != l->start || s->depth != l->depth || s->nchain != l->nchain || !s->nchain) return 0;
    if (!ss_same_continuation(s->ret, l->ret)) return 0;
    for (int k = 1; k < s->nchain; k++)
        if (s->chain[k][0] != l->chain[k][0] || s->chain[k][1] != l->chain[k][1]) return 0;
    return 1;
}
static int match_threads(SSRd *r, GThread **live, int *nlive, SSThread *saved, int *map) {
    int ns = 0;
    if (!SSR(r, ns) || ns < 0 || ns > 64) return 0;
    for (int i = 0; i < ns; i++) if (!SSR(r, saved[i])) return 0;
    *nlive = parked_threads(live);
    SSThread ld[64];
    for (int j = 0; j < *nlive; j++) describe(live[j], &ld[j]);
    int used[64] = {0};
    for (int i = 0; i < ns; i++) {
        map[i] = -1;
        for (int j = 0; j < *nlive && map[i] < 0; j++) if (!used[j] && compatible(&saved[i], &ld[j])) { map[i] = j; used[j] = 1; }
        if (map[i] < 0) { LOG("[ss] load refused: no running thread can continue saved thread start=%08X ret=%08X\n", saved[i].start, saved[i].ret); return 0; }
    }
    return ns + 1;
}
/* Matching reads the live threads' stacks, so it runs before guest memory is replaced; load applies it. */
static struct { int ns, nlive, map[64]; GThread *live[64]; SSThread saved[64]; } g_match;
int k32_ss_check(SSRd r) {
    pthread_mutex_lock(&g_thr_lock);
    int ok = match_threads(&r, g_match.live, &g_match.nlive, g_match.saved, g_match.map);
    g_match.ns = ok ? ok - 1 : -1;
    pthread_mutex_unlock(&g_thr_lock);
    return ok;
}
void k32_ss_load(SSRd *r) {
    (void)r;
    if (g_match.ns < 0) return;
    pthread_mutex_lock(&g_thr_lock);
    int keep[64] = {0};
    for (int i = 0; i < g_match.ns; i++) { g_match.live[g_match.map[i]]->cpu = g_match.saved[i].cpu; keep[g_match.map[i]] = 1; }
    for (int j = 0; j < g_match.nlive; j++)
        if (!keep[j]) { g_match.live[j]->zombie = 1; LOG("[ss] retiring thread start=%08X\n", g_match.live[j]->start); }
    g_match.ns = -1;
    pthread_mutex_unlock(&g_thr_lock);
}

S(CreateThread) {
    uint32_t stack = ARG(c, 1) ? ARG(c, 1) : 0x100000u, start = ARG(c, 2), param = ARG(c, 3), flags = ARG(c, 4), ptid = ARG(c, 5);
    if (stack < 0x40000u) stack = 0x40000u;
    pthread_mutex_lock(&g_thr_lock);
    int slot = -1;
    for (int i = 0; i < 64; i++) if (!g_threads[i]) { slot = i; break; }
    if (slot < 0) { pthread_mutex_unlock(&g_thr_lock); RET(c, 0, 6); return; }
    GThread *t = calloc(1, sizeof *t);
    t->slot = slot;
    g_threads[slot] = t;
    uint32_t tid = g_next_tid += 4;
    pthread_mutex_unlock(&g_thr_lock);
    /* guest stack + TEB (TLS slots live in the TEB) */
    uint32_t base = gvalloc(stack + 0x2000u);
    uint32_t teb = base + stack;
    memcpy(gptr(teb), gptr(GUEST_TEB), 0x1000);
    s32(teb + 0x00, 0xFFFFFFFFu); s32(teb + 0x04, teb); s32(teb + 0x08, base); s32(teb + 0x18, teb); s32(teb + 0x24, tid);
    memset(gptr(teb + 0xE10), 0, 64 * 4);
    t->cpu.fpu_cw = 0x027F; t->cpu.fs_base = teb; t->cpu.esp = teb - 0x40; t->cpu.thread_id = tid;
    t->start = start; t->param = param; t->handle = 0x3000u + (uint32_t)slot;
    t->suspended = (flags & 4) != 0;
    pthread_mutex_init(&t->m, NULL); pthread_cond_init(&t->cv, NULL);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 8u << 20);
    pthread_create(&t->pt, &at, thread_main, t);
    if (ptid) s32(ptid, tid);
    LOG("[k32] CreateThread start=%08X param=%08X%s -> tid %X\n", start, param, t->suspended ? " (suspended)" : "", tid);
    RET(c, t->handle, 6);
}
S(ResumeThread) {
    GThread *t = thread_of(ARG(c, 0));
    uint32_t prev = 0;
    if (t) { pthread_mutex_lock(&t->m); prev = (uint32_t)t->suspended; t->suspended = 0; pthread_cond_signal(&t->cv); pthread_mutex_unlock(&t->m); }
    RET(c, prev, 1);
}
S(SetThreadPriority) { RET(c, 1, 2); }
S(TerminateThread) { LOG("[k32] TerminateThread %08X (ignored)\n", ARG(c, 0)); RET(c, 1, 2); }
S(ExitThread) { pthread_exit(NULL); }
