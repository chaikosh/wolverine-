/* Quick save / quick load: a snapshot of the whole running game, taken between two frames.
   Guest memory, the main CPU and every runtime object that mirrors guest state (threads, files,
   heap, timers, Direct3D, DirectSound, Bink) are captured, so loading resumes exactly where the
   game was. Snapshots live in RAM for the current session. */
#pragma once
#include "runtime.h"
#include <stddef.h>

enum { SS_NONE, SS_SAVE, SS_LOAD };
void ss_request(int op);                 /* from the UI or keyboard; serviced at the next Present */
void ss_present(CPU *c);                 /* called by IDirect3DDevice8::Present once the frame is shown */
int ss_feedback(float *age);             /* last result for the UI: +1 saved, +2 loaded, -1 failed; age in s */
int ss_has_snapshot(void);

/* growable byte buffer for module state, and a reader over it */
typedef struct { uint8_t *p; size_t n, cap; } SSBuf;
typedef struct { const uint8_t *p, *end; } SSRd;
void ssw(SSBuf *b, const void *src, size_t n);
int ssr(SSRd *r, void *dst, size_t n);   /* 0 when the buffer is short */
#define SSW(b, v) ssw((b), &(v), sizeof(v))
#define SSR(r, v) ssr((r), &(v), sizeof(v))

/* call-chain helpers shared with the thread code */
int ss_chain(uint32_t esp, uint32_t top, uint32_t out[][2], int max);   /* return addresses (offset from esp, value) */
int ss_same_continuation(uint32_t ret_a, uint32_t ret_b);              /* both return into the same jmp target */

/* module hooks (save writes state, load restores it; check validates before anything is touched) */
int  k32_park_others(int timeout_ms);    /* stop every other guest thread at a Sleep call */
void k32_unpark_others(void);
void k32_ss_save(SSBuf *b);
int  k32_ss_check(SSRd r);               /* can the saved threads be matched to the live ones? */
void k32_ss_load(SSRd *r);
void k32_time_save(SSBuf *b);
void k32_time_load(SSRd *r);
void files_ss_save(SSBuf *b);
void files_ss_load(SSRd *r);
void d3d_ss_save(SSBuf *b);
void d3d_ss_load(SSRd *r);
void ds_ss_lock(int lock);               /* hold the mixer while guest memory is replaced */
void ds_ss_save(SSBuf *b);
void ds_ss_load(SSRd *r);
void bink_ss_save(SSBuf *b);
void bink_ss_load(SSRd *r);
void plat_ss_flush_input(void);
