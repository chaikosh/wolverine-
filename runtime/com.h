/* Minimal COM: guest-visible objects whose vtables point at host thunks. */
#pragma once
#include "runtime.h"

typedef struct { const char *name; GuestFn fn; } ComMethod;

/* Build a guest vtable. Missing methods trap with "Iface::Method". */
uint32_t com_vtable(const char *iface, const char *const *names, const GuestFn *fns, int n);
/* Allocate a guest object {vtbl, host-slot, refcount} bound to a host pointer. */
uint32_t com_new(uint32_t vtbl, void *host);
void *com_host(uint32_t obj);
uint32_t com_addref(uint32_t obj);
uint32_t com_release(uint32_t obj);   /* returns new refcount; frees guest block at 0 */
void com_unimpl(CPU *c) __attribute__((noreturn));

#define THIS (ARG(c, 0))
#define HR_OK 0u
#define E_NOTIMPL 0x80004001u
#define E_FAIL 0x80004005u
#define E_NOINTERFACE 0x80004002u
