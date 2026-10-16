/* Shared Win32 emulation state. */
#pragma once
#include "../runtime.h"
#include <pthread.h>

#define VIRT_DIR "C:\\X2"          /* virtual install directory reported to the game */

/* modules */
uint32_t module_handle(const char *name);
const char *module_name(uint32_t handle);
uint32_t resolve_export(const char *dll, const char *sym);

/* critical sections */
pthread_mutex_t *cs_get(uint32_t guest_addr);

/* file system */
int host_path(const char *guest, char *out, size_t n, int must_exist);
void host_path_write(const char *guest, char *out, size_t n);
extern char g_write_dir[1024];
const char *file_handle_path(uint32_t handle);
