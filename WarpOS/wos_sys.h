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
/* Bring-up breadcrumbs: [0] the last site passed, [1] the frame, [2] the PSX
 * pc at the last vblank or watchdog look, [3] a site's own value. The line is
 * pushed to memory at every write, so the bridge can read it from the 68k
 * while the program hangs; the address is in the log's first lines. */
extern volatile uint32_t wos_crumb[8];
#ifdef WOS_CRUMBS               /* make WOS_CRUMBS=1: they cost a dcbst+sync each */
#define WOS_CRUMB(n) do { wos_crumb[0] = (n); __asm__ volatile("dcbst 0,%0\n\tsync" : : "r"(wos_crumb) : "memory"); } while (0)
#define WOS_CRUMB2(n, v) do { wos_crumb[3] = (uint32_t)(v); WOS_CRUMB(n); } while (0)
#define WOS_CRUMB_SET(i, v) (wos_crumb[i] = (uint32_t)(v))
#else
#define WOS_CRUMB(n) do { } while (0)
#define WOS_CRUMB2(n, v) do { (void)(v); } while (0)
#define WOS_CRUMB_SET(i, v) do { } while (0)
#endif

/* Bring-up tracing: each WOS_TRACE logs and flushes while the budget lasts. */
extern int wos_trace_budget;
#define WOS_TRACE(...) do { if (wos_trace_budget > 0) { wos_trace_budget--; wos_log(__VA_ARGS__); wos_log_flush(); } } while (0)
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
const char *wos_getenv(const char *name);
int      wos_env_int(const char *name, int def);
int      wos_env_take(const char *name);

#ifdef __cplusplus
}
#endif

#endif
