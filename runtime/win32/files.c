/* KERNEL32 file system, file mapping, INI and resource shims. */
#include "win32.h"
#include "../savestate.h"
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <strings.h>
#include <sys/stat.h>

#define S(name) void shim_KERNEL32_##name(CPU *c)
#define INVALID 0xFFFFFFFFu
#define MAXH 1024

typedef struct { int used, fd, oflags; char path[1024]; char **names; int nnames, pos; char dir[1024]; } HFile;
static HFile g_h[MAXH];

static uint32_t h_new(void) {
    for (int i = 1; i < MAXH; i++)
        if (!g_h[i].used) { memset(&g_h[i], 0, sizeof g_h[i]); g_h[i].used = 1; g_h[i].fd = -1; return 0x1000u + (uint32_t)i * 4; }
    LOG("[file] handle table full\n");
    return INVALID;
}
static int h_count(void) { int n = 0; for (int i = 1; i < MAXH; i++) n += g_h[i].used; return n; }
static HFile *h_get(uint32_t h) { uint32_t i = (h - 0x1000u) / 4; return (h >= 0x1000u && i < MAXH && g_h[i].used) ? &g_h[i] : NULL; }
const char *file_handle_path(uint32_t h) { HFile *f = h_get(h); return f ? f->path : NULL; }
static void set_err(CPU *c, uint32_t e) { s32(c->fs_base + 0x34, e); }

static uint32_t open_file(CPU *c, const char *name, uint32_t access, uint32_t disp) {
    char hp[1024];
    int exists = host_path(name, hp, sizeof hp, 1);
    int fl = (access & 0x40000000u) ? ((access & 0x80000000u) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (fl != O_RDONLY || disp == 1 || disp == 2) {   /* writing: redirect into the writable directory */
        char wp[1024];
        host_path_write(name, wp, sizeof wp);
        if (exists && strcmp(wp, hp) && disp != 2 && disp != 5) {   /* copy-on-write from the read-only tree */
            FILE *a = fopen(hp, "rb"), *b = fopen(wp, "wb");
            if (a && b) { char buf[65536]; size_t k; while ((k = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, k, b); }
            if (a) fclose(a);
            if (b) fclose(b);
        }
        snprintf(hp, sizeof hp, "%s", wp);
    }
    switch (disp) {
    case 1: if (exists) { set_err(c, 80); return INVALID; } fl |= O_CREAT; break;   /* CREATE_NEW */
    case 2: fl |= O_CREAT | O_TRUNC; break;                                          /* CREATE_ALWAYS */
    case 3: break;                                                                   /* OPEN_EXISTING */
    case 4: fl |= O_CREAT; break;                                                    /* OPEN_ALWAYS */
    case 5: fl |= O_TRUNC; break;                                                    /* TRUNCATE_EXISTING */
    }
    int fd = open(hp, fl, 0644);
    if (g_trace_imports || fd < 0) LOG("[file] open %s -> %s %s\n", name, hp, fd < 0 ? "FAILED" : "ok");
    if (fd < 0) { set_err(c, 2); return INVALID; }
    uint32_t h = h_new();
    HFile *f = h_get(h);
    if (!f) { close(fd); set_err(c, 4); return INVALID; }
    f->fd = fd;
    f->oflags = fl & ~(O_CREAT | O_TRUNC | O_EXCL);   /* how to reopen it after a quick load */
    snprintf(f->path, sizeof f->path, "%s", hp);
    set_err(c, 0);
    return h;
}
/* Save states: open files come back with the same handle, path and position. */
void files_ss_save(SSBuf *b) {
    for (int i = 1; i < MAXH; i++) {
        HFile *f = &g_h[i];
        if (!f->used || f->fd < 0) continue;
        int64_t pos = lseek(f->fd, 0, SEEK_CUR);
        uint16_t len = (uint16_t)strlen(f->path);
        SSW(b, i); SSW(b, f->oflags); SSW(b, pos); SSW(b, len); ssw(b, f->path, len);
    }
    int end = 0; SSW(b, end);
}
static void h_close(HFile *f) {
    if (f->fd >= 0) close(f->fd);
    for (int k = 0; k < f->nnames; k++) free(f->names[k]);
    free(f->names);
    memset(f, 0, sizeof *f);
    f->fd = -1;
}
void files_ss_load(SSRd *r) {
    static char want[MAXH];
    memset(want, 0, sizeof want);
    for (;;) {
        int i = 0, oflags = 0; int64_t pos = 0; uint16_t len = 0; char path[1024];
        if (!SSR(r, i) || i <= 0 || i >= MAXH) break;
        SSR(r, oflags); SSR(r, pos); SSR(r, len);
        if (len >= sizeof path || !ssr(r, path, len)) break;
        path[len] = 0;
        HFile *f = &g_h[i];
        want[i] = 1;
        if (!(f->used && f->fd >= 0 && !strcmp(f->path, path))) {
            if (f->used) h_close(f);
            f->fd = open(path, oflags);
            if (f->fd < 0) { LOG("[ss] could not reopen %s\n", path); continue; }
            f->used = 1; f->oflags = oflags;
            snprintf(f->path, sizeof f->path, "%s", path);
        }
        lseek(f->fd, pos, SEEK_SET);
    }
    for (int i = 1; i < MAXH; i++) if (!want[i] && g_h[i].used && g_h[i].fd >= 0) h_close(&g_h[i]);   /* opened after the save */
}

S(CreateFileA) { RET(c, open_file(c, gstr(ARG(c, 0)), ARG(c, 1), ARG(c, 4)), 7); }
S(CreateFileW) {
    char n[1024]; uint32_t p = ARG(c, 0); int i = 0;
    for (; g16(p + 2 * (uint32_t)i) && i < 1023; i++) n[i] = (char)g16(p + 2 * (uint32_t)i);
    n[i] = 0;
    RET(c, open_file(c, n, ARG(c, 1), ARG(c, 4)), 7);
}
S(ReadFile) {
    HFile *f = h_get(ARG(c, 0));
    uint32_t buf = ARG(c, 1), n = ARG(c, 2), pr = ARG(c, 3);
    if (!f || f->fd < 0) { if (pr) s32(pr, 0); set_err(c, 6); RET(c, 0, 5); return; }
    ssize_t r = read(f->fd, gptr(buf), n);
    if (pr) s32(pr, r < 0 ? 0 : (uint32_t)r);
    RET(c, r >= 0, 5);
}
S(WriteFile) {
    uint32_t h = ARG(c, 0), buf = ARG(c, 1), n = ARG(c, 2), pw = ARG(c, 3);
    if (h >= 0x10000u && h <= 0x1000Cu) { fwrite(gptr(buf), 1, n, stderr); if (pw) s32(pw, n); RET(c, 1, 5); return; }
    HFile *f = h_get(h);
    ssize_t r = f ? write(f->fd, gptr(buf), n) : -1;
    if (pw) s32(pw, r < 0 ? 0 : (uint32_t)r);
    RET(c, r >= 0, 5);
}
S(SetFilePointer) {
    HFile *f = h_get(ARG(c, 0));
    int64_t off = (int32_t)ARG(c, 1);
    uint32_t phi = ARG(c, 2);
    if (phi) off = (int64_t)(((uint64_t)g32(phi) << 32) | ARG(c, 1));
    if (!f) { RET(c, INVALID, 4); return; }
    off_t r = lseek(f->fd, off, (int)ARG(c, 3));
    if (phi) s32(phi, (uint32_t)((uint64_t)r >> 32));
    RET(c, (uint32_t)r, 4);
}
S(GetFileSize) {
    HFile *f = h_get(ARG(c, 0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) { RET(c, INVALID, 2); return; }
    if (ARG(c, 1)) s32(ARG(c, 1), (uint32_t)((uint64_t)st.st_size >> 32));
    RET(c, (uint32_t)st.st_size, 2);
}
S(GetFileType) { uint32_t h = ARG(c, 0); RET(c, (h >= 0x10000u && h <= 0x1000Cu) ? 2 : (h_get(h) ? 1 : 0), 1); }
S(CloseHandle) {
    if (getenv("TRACE_HANDLES")) { static int k; if (++k % 200 == 0) LOG("[file] handles in use: %d\n", h_count()); }
    HFile *f = h_get(ARG(c, 0));
    if (f) { if (f->fd >= 0) close(f->fd); for (int i = 0; i < f->nnames; i++) free(f->names[i]); free(f->names); f->used = 0; }
    RET(c, 1, 1);
}
S(FlushFileBuffers) { RET(c, 1, 1); }
S(SetEndOfFile) { HFile *f = h_get(ARG(c, 0)); RET(c, f && !ftruncate(f->fd, lseek(f->fd, 0, SEEK_CUR)), 1); }
S(DeleteFileA) { char hp[1024]; host_path_write(gstr(ARG(c, 0)), hp, sizeof hp); RET(c, unlink(hp) == 0, 1); }
S(MoveFileA) { char a[1024], b[1024]; host_path_write(gstr(ARG(c, 0)), a, sizeof a); host_path_write(gstr(ARG(c, 1)), b, sizeof b); RET(c, rename(a, b) == 0, 2); }
S(CreateDirectoryA) { char hp[1024]; host_path_write(gstr(ARG(c, 0)), hp, sizeof hp); RET(c, mkdir(hp, 0755) == 0, 2); }
S(GetCurrentDirectoryA) {
    uint32_t n = ARG(c, 0), buf = ARG(c, 1), len = (uint32_t)strlen(VIRT_DIR);
    if (n > len) strcpy((char *)gptr(buf), VIRT_DIR);
    RET(c, n > len ? len : len + 1, 2);
}
S(GetDiskFreeSpaceExA) {
    for (int k = 1; k <= 3; k++) if (ARG(c, k)) { s32(ARG(c, k), 0); s32(ARG(c, k) + 4, 2); }   /* 8 GiB */
    RET(c, 1, 4);
}

/* Find handles live in their own ring: Wolvie.exe scans the save slots every frame and never calls FindClose,
   which Windows tolerates; here the oldest search is simply recycled. */
#define MAXFIND 64
static HFile g_find[MAXFIND];
static int g_find_next;
static uint32_t find_new(void) {
    int i = g_find_next++ % MAXFIND;
    HFile *f = &g_find[i];
    for (int k = 0; k < f->nnames; k++) free(f->names[k]);
    free(f->names);
    memset(f, 0, sizeof *f);
    f->used = 1; f->fd = -1;
    return 0x8000u + (uint32_t)i * 4;
}
static HFile *find_get(uint32_t h) { uint32_t i = (h - 0x8000u) / 4; return (h >= 0x8000u && i < MAXFIND && g_find[i].used) ? &g_find[i] : NULL; }

/* FindFirstFile: WIN32_FIND_DATAA is 320 bytes, name at +44 */
static void fill_find(HFile *f, uint32_t data) {
    memset(gptr(data), 0, 320);
    char full[2048];
    snprintf(full, sizeof full, "%s/%s", f->dir, f->names[f->pos]);
    struct stat st;
    if (!stat(full, &st)) {
        s32(data, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
        s32(data + 32, (uint32_t)((uint64_t)st.st_size >> 32));
        s32(data + 28 + 4, (uint32_t)st.st_size);
    }
    snprintf((char *)gptr(data + 44), 260, "%s", f->names[f->pos]);
}
S(FindFirstFileA) {
    const char *pat = gstr(ARG(c, 0));
    char dir[1024], hp[1024];
    const char *slash = strrchr(pat, '\\');
    if (slash) { snprintf(dir, sizeof dir, "%.*s", (int)(slash - pat), pat); slash++; }
    else { strcpy(dir, "."); slash = pat; }
    host_path(dir, hp, sizeof hp, 1);
    uint32_t h = find_new();
    HFile *f = find_get(h);
    snprintf(f->dir, sizeof f->dir, "%s", hp);
    DIR *d = opendir(hp);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
            if (fnmatch(slash, e->d_name, FNM_CASEFOLD) == 0 || !strcmp(slash, "*.*")) {
                f->names = realloc(f->names, sizeof(char *) * (size_t)(f->nnames + 1));
                f->names[f->nnames++] = strdup(e->d_name);
            }
        }
        closedir(d);
    }
    if (g_trace_imports) LOG("[file] find %s -> %d matches in %s\n", pat, f->nnames, hp);
    if (getenv("TRACE_HANDLES")) { static int k; if (++k % 100 == 0) LOG("[file] finds=%d handles in use: %d\n", k, h_count()); }
    if (!f->nnames) { f->used = 0; set_err(c, 2); RET(c, INVALID, 2); return; }
    fill_find(f, ARG(c, 1));
    RET(c, h, 2);
}
S(FindNextFileA) {
    HFile *f = find_get(ARG(c, 0));
    if (!f || f->pos + 1 >= f->nnames) { set_err(c, 18); RET(c, 0, 2); return; }
    f->pos++;
    fill_find(f, ARG(c, 1));
    RET(c, 1, 2);
}
S(FindClose) {
    if (getenv("TRACE_HANDLES")) { static int k; if (++k % 100 == 0) LOG("[file] FindClose calls=%d (handle %08X)\n", k, ARG(c, 0)); }
    HFile *f = find_get(ARG(c, 0)); if (f) { for (int i = 0; i < f->nnames; i++) free(f->names[i]); free(f->names); f->names = NULL; f->nnames = 0; f->used = 0; } RET(c, 1, 1); }

/* File mapping: whole-file copy into guest heap */
S(CreateFileMappingA) { RET(c, ARG(c, 0) | 0x80000000u, 6); }
S(MapViewOfFile) {
    HFile *f = h_get(ARG(c, 0) & 0x7FFFFFFFu);
    struct stat st;
    if (!f || fstat(f->fd, &st)) { RET(c, 0, 5); return; }
    uint32_t off = ARG(c, 3), n = ARG(c, 4) ? ARG(c, 4) : (uint32_t)st.st_size - off;
    uint32_t p = galloc(n, 0);
    pread(f->fd, gptr(p), n, off);
    RET(c, p, 5);
}
S(UnmapViewOfFile) { gfree(ARG(c, 0)); RET(c, 1, 1); }

/* Resources are not used for gameplay; report "not found". */
S(FindResourceA) { RET(c, 0, 3); }
S(FindResourceW) { RET(c, 0, 3); }
S(LoadResource) { RET(c, 0, 2); }
S(LockResource) { RET(c, ARG(c, 0), 1); }
S(SizeofResource) { RET(c, 0, 2); }

/* ------------------------------------------------------------ INI files */
static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    b[fread(b, 1, (size_t)n, f)] = 0;
    fclose(f);
    return b;
}
static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s; while (isspace((unsigned char)*b)) b++;
    memmove(s, b, strlen(b) + 1);
}
/* Look up app/key; returns 1 and copies value if found. */
static int ini_get(const char *file, const char *app, const char *key, char *out, size_t n) {
    char hp[1024];
    host_path(file, hp, sizeof hp, 1);
    char *buf = read_all(hp);
    if (!buf) return 0;
    int in = 0, found = 0;
    for (char *save, *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char l[512]; snprintf(l, sizeof l, "%s", line); trim(l);
        if (l[0] == '[') { char *e = strchr(l, ']'); if (e) *e = 0; in = !strcasecmp(l + 1, app); continue; }
        char *eq = strchr(l, '=');
        if (in && eq) { *eq = 0; trim(l); if (!strcasecmp(l, key)) { char *v = eq + 1; trim(v); snprintf(out, n, "%s", v); found = 1; break; } }
    }
    free(buf);
    return found;
}
S(GetPrivateProfileIntA) {
    char v[64];
    int found = ini_get(gstr(ARG(c, 3)), gstr(ARG(c, 0)), gstr(ARG(c, 1)), v, sizeof v);
    if (getenv("TRACE_INI")) LOG("[ini] get [%s] %s -> %s\n", gstr(ARG(c, 0)), gstr(ARG(c, 1)), found ? v : "(default)");
    RET(c, found ? (uint32_t)strtol(v, NULL, 10) : ARG(c, 2), 4);
}
S(GetPrivateProfileStringA) {
    const char *app = gstr(ARG(c, 0)), *key = gstr(ARG(c, 1)), *def = gstr(ARG(c, 2));
    uint32_t buf = ARG(c, 3), n = ARG(c, 4);
    char v[1024];
    if (!app || !key) { if (n >= 2) { gptr(buf)[0] = 0; gptr(buf)[1] = 0; } RET(c, 0, 6); return; }
    if (!ini_get(gstr(ARG(c, 5)), app, key, v, sizeof v)) snprintf(v, sizeof v, "%s", def ? def : "");
    uint32_t len = (uint32_t)strlen(v);
    if (len >= n) len = n - 1;
    memcpy(gptr(buf), v, len); gptr(buf)[len] = 0;
    RET(c, len, 6);
}
S(WritePrivateProfileStringA) {
    const char *app = gstr(ARG(c, 0)), *key = gstr(ARG(c, 1)), *val = gstr(ARG(c, 2));
    if (getenv("TRACE_INI")) LOG("[ini] set [%s] %s = %s (%s)\n", app, key ? key : "-", val ? val : "-", gstr(ARG(c, 3)));
    char hp[1024], wp[1024];
    host_path(gstr(ARG(c, 3)), hp, sizeof hp, 0);
    host_path_write(gstr(ARG(c, 3)), wp, sizeof wp);
    char *buf = read_all(hp);
    size_t cap = (buf ? strlen(buf) : 0) + 1024;
    char *out = malloc(cap); out[0] = 0;
    int in = 0, done = 0, saw_app = 0;
    if (buf) {
        for (char *save, *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
            char l[512]; snprintf(l, sizeof l, "%s", line); trim(l);
            if (l[0] == '[') {
                if (in && !done && key && val) { strcat(out, key); strcat(out, "="); strcat(out, val); strcat(out, "\r\n"); done = 1; }
                char nm[512]; snprintf(nm, sizeof nm, "%s", l + 1); char *e = strchr(nm, ']'); if (e) *e = 0;
                in = !strcasecmp(nm, app); saw_app |= in;
            } else if (in && key) {
                char k[512]; snprintf(k, sizeof k, "%s", l); char *eq = strchr(k, '=');
                if (eq) { *eq = 0; trim(k); if (!strcasecmp(k, key)) { if (val && !done) { strcat(out, key); strcat(out, "="); strcat(out, val); strcat(out, "\r\n"); } done = 1; continue; } }
            }
            strcat(out, line); strcat(out, "\r\n");
        }
    }
    if (!done && key && val) {
        if (!in) { if (!saw_app) { strcat(out, "["); strcat(out, app); strcat(out, "]\r\n"); } }
        strcat(out, key); strcat(out, "="); strcat(out, val); strcat(out, "\r\n");
    }
    FILE *f = fopen(wp, "wb");
    if (f) { fputs(out, f); fclose(f); }
    free(buf); free(out);
    RET(c, f != NULL, 4);
}
