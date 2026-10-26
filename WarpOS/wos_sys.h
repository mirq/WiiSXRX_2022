/*
 * wos_sys.h -- the WarpOS build's clock, cache, log and library setup.
 * Plain C types only, so any emulator file can include it.
 */
#ifndef WOS_SYS_H
#define WOS_SYS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int      wos_sys_init(void);          /* open the libraries; 0 on success */
void     wos_sys_shutdown(void);

uint64_t wos_now_us(void);            /* the PPC's own clock; never leaves the PPC */
void     wos_wait_until_us(uint64_t t);  /* spin: WaitTime oversleeps by ~13 ms */

/* Make freshly written code visible to the instruction fetch. */
void     wos_cache_flush_code(void *start, uint32_t len);
/* Push data out of the PPC's cache for the 68k (AHI) / drop stale lines. */
void     wos_cache_flush_data(void *start, uint32_t len);
void     wos_cache_invalidate_data(void *start, uint32_t len);

/* The log: collected in memory, written every 2 s and at exit. */
void     wos_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void     wos_log_flush(void);
void     wos_log_tick(void);          /* once a frame: writes when 2 s have passed */

/* Memory the PPC executes and reads fastest: its own RAM, 32-byte aligned. */
void    *wos_alloc_ppc(uint32_t size);
void     wos_free_ppc(void *p);

/* Run from the program's own drawer: Lock("PROGDIR:") + CurrentDir, never
 * newlib chdir (it unlocks the Shell's own lock). */
void     wos_enter_progdir(void);
void     wos_leave_progdir(void);

/* A path relative to the current directory made absolute (Lock +
 * NameFromLock), before wos_enter_progdir changes what it is relative to. */
void     wos_make_absolute(char *path, unsigned size);

/* An ASL file requester for a disc image; 1 with the path in buf. */
int      wos_request_image(char *buf, unsigned size);

/* A ENV: variable as an int, or def when unset. */
int      wos_env_int(const char *name, int def);

#ifdef __cplusplus
}
#endif

#endif
