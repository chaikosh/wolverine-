/* Modules, exports, critical sections, path mapping. */
#include "win32.h"
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <strings.h>
#include <sys/stat.h>

/* ------------------------------------------------------------ modules */
static char g_mod_names[64][32];
static int g_mod_count;

static void norm_module(const char *in, char *out) {
    const char *b = strrchr(in, '\\'); b = b ? b + 1 : in;
    const char *b2 = strrchr(b, '/'); b = b2 ? b2 + 1 : b;
    int i = 0;
    for (; b[i] && i < 31; i++) out[i] = (char)toupper((unsigned char)b[i]);
    out[i] = 0;
    char *dot = strrchr(out, '.');
    if (dot && !strcmp(dot, ".DLL")) *dot = 0;
}

uint32_t module_handle(const char *name) {
    char n[32];
    norm_module(name, n);
    if (!strcmp(n, "WOLVIE.EXE")) return g_image_base;
    for (int i = 0; i < g_mod_count; i++)
        if (!strcmp(g_mod_names[i], n)) return 0x70000000u + (uint32_t)i * 0x10000u;
    if (g_mod_count >= 64) return 0;
    strcpy(g_mod_names[g_mod_count], n);
    return 0x70000000u + (uint32_t)g_mod_count++ * 0x10000u;
}

const char *module_name(uint32_t h) {
    if (h == g_image_base) return "WOLVIE";
    uint32_t i = (h - 0x70000000u) / 0x10000u;
    return (h >= 0x70000000u && (int)i < g_mod_count) ? g_mod_names[i] : "?";
}

uint32_t resolve_export(const char *dll, const char *sym) {
    static struct { char key[160]; uint32_t addr; } cache[512];
    static int ncache;
    char key[160], fn[200];
    snprintf(key, sizeof key, "%s!%s", dll, sym);
    for (int i = 0; i < ncache; i++) if (!strcmp(cache[i].key, key)) return cache[i].addr;
    char s[128]; int k = 0;
    for (const char *p = sym; *p && k < 127; p++) s[k++] = isalnum((unsigned char)*p) ? *p : '_';
    s[k] = 0;
    snprintf(fn, sizeof fn, "shim_%s_%s", dll, s);
    GuestFn f = (GuestFn)dlsym(RTLD_DEFAULT, fn);
    if (!f) {
        if (!strstr(sym, "DebugSetMute")) LOG("[k32] GetProcAddress(%s): not implemented\n", key);
        return 0;
    }
    uint32_t a = rt_register_thunk(f, strdup(key));
    if (ncache < 512) { strcpy(cache[ncache].key, key); cache[ncache++].addr = a; }
    return a;
}

/* ------------------------------------------------------------ critical sections */
static struct { uint32_t key; pthread_mutex_t *m; } g_cs[4096];
static pthread_mutex_t g_cs_lock = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t *cs_get(uint32_t a) {
    pthread_mutex_lock(&g_cs_lock);
    uint32_t h = (a >> 2) & 4095;
    while (g_cs[h].key && g_cs[h].key != a) h = (h + 1) & 4095;
    if (!g_cs[h].key) {
        pthread_mutexattr_t at;
        pthread_mutexattr_init(&at);
        pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
        g_cs[h].m = malloc(sizeof(pthread_mutex_t));
        pthread_mutex_init(g_cs[h].m, &at);
        g_cs[h].key = a;
    }
    pthread_mutex_t *m = g_cs[h].m;
    pthread_mutex_unlock(&g_cs_lock);
    return m;
}

/* ------------------------------------------------------------ paths */
char g_write_dir[1024];

/* Strip the virtual install dir / drive letter and convert separators. */
static void rel_path(const char *guest, char *out, size_t n) {
    const char *p = guest;
    size_t vl = strlen(VIRT_DIR);
    if (!strncasecmp(p, VIRT_DIR, vl) && (p[vl] == '\\' || p[vl] == '/' || !p[vl])) p += vl;
    else if (isalpha((unsigned char)p[0]) && p[1] == ':') p += 2;
    while (*p == '\\' || *p == '/') p++;
    while (p[0] == '.' && (p[1] == '\\' || p[1] == '/')) p += 2;
    snprintf(out, n, "%s", p);
    for (char *q = out; *q; q++) if (*q == '\\') *q = '/';
}

/* Resolve rel under root, matching each component case-insensitively. Returns 1 if it exists. */
static int resolve(const char *root, const char *rel, char *out, size_t n, int mkdirs) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", rel);
    snprintf(out, n, "%s", root);
    int ok = 1;
    char *save, *comp = strtok_r(tmp, "/", &save);
    while (comp) {
        char *next = strtok_r(NULL, "/", &save);
        char cand[1024];
        snprintf(cand, sizeof cand, "%s/%s", out, comp);
        struct stat st;
        if (ok && stat(cand, &st) != 0) {
            DIR *d = opendir(out);
            int found = 0;
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)))
                    if (!strcasecmp(e->d_name, comp)) { snprintf(cand, sizeof cand, "%s/%s", out, e->d_name); found = 1; break; }
                closedir(d);
            }
            if (!found) {
                ok = 0;
                if (mkdirs && next) mkdir(cand, 0755);
            }
        } else if (!ok && mkdirs && next) mkdir(cand, 0755);
        snprintf(out, n, "%s", cand);
        comp = next;
    }
    return ok;
}

/* Read lookup: writable overlay first (saves, edited INI), then the game directory. */
int host_path(const char *guest, char *out, size_t n, int must_exist) {
    char rel[1024];
    rel_path(guest, rel, sizeof rel);
    if (g_write_dir[0] && strcmp(g_write_dir, g_game_dir) && resolve(g_write_dir, rel, out, n, 0)) return 1;
    int ok = resolve(g_game_dir, rel, out, n, 0);
    return must_exist ? ok : 1;
}

/* Write target: always inside the writable directory; parent directories are created. */
void host_path_write(const char *guest, char *out, size_t n) {
    char rel[1024];
    rel_path(guest, rel, sizeof rel);
    resolve(g_write_dir[0] ? g_write_dir : g_game_dir, rel, out, n, 1);
}
