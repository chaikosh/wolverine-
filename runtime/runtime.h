/* Runtime services shared by the loader, dispatcher and Win32 shims. */
#pragma once
#include "cpu.h"
#include <stdio.h>

typedef struct { uint32_t addr; GuestFn fn; } FuncEntry;
typedef struct { const char *dll, *name; uint32_t iat; } ImportDesc;

extern const FuncEntry g_func_table[];
extern const uint32_t g_func_count;
extern const ImportDesc g_import_desc[];
extern const uint32_t g_import_count;
extern const GuestFn g_import_stubs[];
extern const uint32_t g_image_base, g_image_size, g_entry_point;

/* Guest address map: everything lives in a 1 GiB window (GMASK in cpu.h) so iOS accepts the reservation. */
#define GUEST_SPACE       0x40000000u
#define GUEST_STACK_TOP   0x00400000u   /* main thread stack grows down from here */
#define GUEST_STACK_SIZE  0x00200000u
#define GUEST_HEAP_BASE   0x01000000u   /* HeapAlloc arena (~624 MiB) */
#define GUEST_HEAP_END    0x28000000u
#define GUEST_VA_BASE     0x28000000u   /* VirtualAlloc arena (~368 MiB) */
#define GUEST_VA_END      0x3F000000u
#define GUEST_TEB         0x3FFDE000u
#define GUEST_PEB         0x3FFDF000u
#define GUEST_SCRATCH     0x3FFE0000u   /* runtime-owned strings/structures */
#define THUNK_BASE        0xFE000000u
#define THUNK_STRIDE      16u
#define THUNK_MAX         65536u

uint32_t rt_register_thunk(GuestFn fn, const char *name);   /* host fn -> callable guest address */
extern __thread uint32_t g_cur_thunk;                       /* thunk being executed (for diagnostics) */
const char *rt_thunk_name(uint32_t addr);
const char *rt_func_name(uint32_t addr);
void rt_unimpl_import(CPU *c, int idx) __attribute__((noreturn));
void rt_trap(CPU *c, uint32_t addr, const char *what) __attribute__((noreturn));
void rt_backtrace(CPU *c);
uint32_t rt_call_guest(CPU *c, uint32_t fn, int nargs, ...);  /* host -> guest stdcall/cdecl */

/* guest memory accessors for host code */
static inline uint8_t *gptr(uint32_t a) { return g_mem + (a & GMASK); }
static inline uint32_t g32(uint32_t a) { return *(u32u *)gptr(a); }
static inline void s32(uint32_t a, uint32_t v) { *(u32u *)gptr(a) = v; }
static inline uint16_t g16(uint32_t a) { return *(u16u *)gptr(a); }
static inline void s16(uint32_t a, uint16_t v) { *(u16u *)gptr(a) = v; }
static inline const char *gstr(uint32_t a) { return a ? (const char *)gptr(a) : NULL; }

/* stdcall shim helpers: args start at [esp+4] */
static inline uint32_t ARG(CPU *c, int n) { return g32(c->esp + 4 + 4 * (uint32_t)n); }
static inline void RET(CPU *c, uint32_t v, int nargs) { c->eax = v; c->esp += 4 + 4 * (uint32_t)nargs; }

/* guest heap */
void gheap_init(void);
uint32_t galloc(uint32_t size, int zero);
void gfree(uint32_t p);
uint32_t gsize(uint32_t p);
uint32_t grealloc(uint32_t p, uint32_t size);
uint32_t gvalloc(uint32_t size);                 /* page-granular, zeroed */
uint32_t gstrdup(const char *s);

extern int g_trace_imports;
extern char g_game_dir[1024];
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); } while (0)
