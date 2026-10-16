#include "com.h"
#include <stdlib.h>

static void *g_hosts[65536];
static uint32_t g_nhosts = 1;

void com_unimpl(CPU *c) {
    rt_trap(c, g32(c->esp), rt_thunk_name(g_cur_thunk) ? rt_thunk_name(g_cur_thunk) : "COM method");
}

uint32_t com_vtable(const char *iface, const char *const *names, const GuestFn *fns, int n) {
    uint32_t vt = galloc(4u * (uint32_t)n, 1);
    for (int i = 0; i < n; i++) {
        char *label = malloc(strlen(iface) + strlen(names[i]) + 4);
        sprintf(label, "%s::%s", iface, names[i]);
        s32(vt + 4u * (uint32_t)i, rt_register_thunk(fns[i] ? fns[i] : com_unimpl, label));
    }
    return vt;
}

uint32_t com_new(uint32_t vtbl, void *host) {
    uint32_t o = galloc(16, 1);
    s32(o, vtbl);
    s32(o + 4, g_nhosts);
    s32(o + 8, 1);
    g_hosts[g_nhosts++ & 0xFFFF] = host;
    return o;
}

void *com_host(uint32_t obj) { return obj ? g_hosts[g32(obj + 4) & 0xFFFF] : NULL; }
uint32_t com_addref(uint32_t obj) { uint32_t r = g32(obj + 8) + 1; s32(obj + 8, r); return r; }
uint32_t com_release(uint32_t obj) {
    uint32_t r = g32(obj + 8);
    if (r) r--;
    s32(obj + 8, r);
    return r;
}
