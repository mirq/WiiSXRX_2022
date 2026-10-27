/*
 * wos_sys.c -- the WarpOS build's clock, caches, log and library bases.
 *
 * Every OS call from PPC code is a trip to the 68k and back with a full cache
 * flush each way, so the things a frame needs often -- the clock, the waits,
 * the cache maintenance -- stay on the PPC: GetSysTimePPC reads the timebase
 * inside powerpc.library, the waits spin, and the caches are handled with the
 * PPC's own dcbst/dcbf/icbi.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "wos_sys.h"
#include "wos_amiga.h"

#ifndef WOS_BUILD_STAMP
#define WOS_BUILD_STAMP "unknown"
#endif

struct IntuitionBase *IntuitionBase;
struct GfxBase       *GfxBase;
struct Library       *AslBase;
struct Library       *LowLevelBase;
extern struct Library *PowerPCBase;

#define CACHE_LINE 32

/* ---- The clock -------------------------------------------------------- */

/* The PPC's time base, read without leaving the PPC. GetSysTimePPC has
 * crashed from a MOS2WOS PPC task on the A4000/MPC7410 (quake3amiga's
 * finding), so the time base is the default; its rate (bus/4, about 24.74 MHz
 * there) is measured against DateStamp about once a second, as quake3amiga's
 * Sys_Milliseconds does. SetEnv WSX_CLOCK sys selects GetSysTimePPC. */
static uint64_t read_timebase(void)
{
    uint32_t hi, lo, hi2;
    do {
        __asm__ volatile("mftbu %0" : "=r"(hi));
        __asm__ volatile("mftb  %0" : "=r"(lo));
        __asm__ volatile("mftbu %0" : "=r"(hi2));
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

static int64_t datestamp_ticks(void)        /* 1/50 s */
{
    struct DateStamp ds;
    DateStamp(&ds);
    return (int64_t)ds.ds_Days * 4320000 + (int64_t)ds.ds_Minute * 3000 + ds.ds_Tick;
}

static int      s_clock_sys = -1;
static uint64_t s_tb_hz = 24740000u;
static uint64_t s_tb_last, s_tb_cal0, s_tb_cal_last;
static int64_t  s_ds_cal0;
static uint64_t s_us, s_us_rem;             /* rem: in 1/1000000 time-base ticks */

uint64_t wos_now_us(void)
{
    uint64_t tb, step;

    if (s_clock_sys < 0) {
        const char *v = wos_getenv("WSX_CLOCK");
        s_clock_sys = (v && !strcmp(v, "sys")) ? 1 : 0;
    }
    if (s_clock_sys) {
        struct timeval tv;
        GetSysTimePPC(&tv);
        return (uint64_t)tv.tv_secs * 1000000u + tv.tv_micro;
    }

    tb = read_timebase();
    if (!s_tb_last) {
        s_tb_last = s_tb_cal0 = s_tb_cal_last = tb;
        s_ds_cal0 = datestamp_ticks();
        return 0;
    }
    if (tb - s_tb_cal_last >= s_tb_hz) {
        int64_t ds = datestamp_ticks() - s_ds_cal0;
        s_tb_cal_last = tb;
        if (ds >= 50) {
            uint64_t hz = (tb - s_tb_cal0) * 50u / (uint64_t)ds;
            if (hz >= 15000000u && hz <= 100000000u)
                s_tb_hz = hz;
        }
    }
    step = (tb - s_tb_last) * 1000000u + s_us_rem;
    s_tb_last = tb;
    s_us += step / s_tb_hz;
    s_us_rem = step % s_tb_hz;
    return s_us;
}

void wos_wait_until_us(uint64_t t)
{
    while (wos_now_us() < t)
        ;
}

/* ---- Caches ----------------------------------------------------------- */

void wos_cache_flush_code(void *start, uint32_t len)
{
    uintptr_t p   = (uintptr_t)start & ~(uintptr_t)(CACHE_LINE - 1);
    uintptr_t end = (uintptr_t)start + len;
    uintptr_t q;

    for (q = p; q < end; q += CACHE_LINE)
        __asm__ volatile("dcbst 0,%0" : : "r"(q) : "memory");
    __asm__ volatile("sync" : : : "memory");
    for (q = p; q < end; q += CACHE_LINE)
        __asm__ volatile("icbi 0,%0" : : "r"(q) : "memory");
    __asm__ volatile("sync; isync" : : : "memory");
}

void wos_cache_flush_data(void *start, uint32_t len)
{
    uintptr_t p   = (uintptr_t)start & ~(uintptr_t)(CACHE_LINE - 1);
    uintptr_t end = (uintptr_t)start + len;

    for (; p < end; p += CACHE_LINE)
        __asm__ volatile("dcbf 0,%0" : : "r"(p) : "memory");
    __asm__ volatile("sync" : : : "memory");
}

void wos_cache_invalidate_data(void *start, uint32_t len)
{
    /* dcbf, not dcbi: a partial line at either end may hold our own writes. */
    wos_cache_flush_data(start, len);
}

/* ---- Memory ----------------------------------------------------------- */

void *wos_alloc_ppc(uint32_t size)
{
    return AllocVecPPC(size, MEMF_PUBLIC | MEMF_CLEAR, CACHE_LINE);
}

void wos_free_ppc(void *p)
{
    if (p)
        FreeVecPPC(p);
}

/* ---- Environment ------------------------------------------------------ */

/* ENV: through dos.library: newlib's getenv() finds nothing under MOS2WOS.
 * The value lives in a static buffer until the next call. */
const char *wos_getenv(const char *name)
{
    static char buf[256];
    if (GetVar((CONST_STRPTR)name, (STRPTR)buf, sizeof(buf), 0) < 0)
        return NULL;
    return buf;
}

/* 1 when the variable is set, which also deletes it: a one-shot request. */
int wos_env_take(const char *name)
{
    char buf[4];
    if (GetVar((CONST_STRPTR)name, (STRPTR)buf, sizeof(buf), 0) < 0)
        return 0;
    DeleteVar((CONST_STRPTR)name, GVF_GLOBAL_ONLY);
    DeleteVar((CONST_STRPTR)name, GVF_LOCAL_ONLY);
    return 1;
}

int wos_env_int(const char *name, int def)
{
    const char *v = wos_getenv(name);
    return (v && *v) ? atoi(v) : def;
}

/* ---- The log ---------------------------------------------------------- */

/* A log line per event written straight to disk cost OpenSWOS 60 ms a frame:
 * the lines are collected here and written out every 2 s and at exit. */
#define LOG_BUF_SIZE (64 * 1024)
#define LOG_PATH     "PROGDIR:wiistation.log"

static char     s_log_buf[LOG_BUF_SIZE];
static uint32_t s_log_len;
static uint64_t s_log_last_us;
static int      s_log_started;

void wos_log_flush(void)
{
    FILE *f;

    if (!s_log_len)
        return;
    f = fopen(LOG_PATH, s_log_started ? "a" : "w");
    if (f) {
        fwrite(s_log_buf, 1, s_log_len, f);
        fclose(f);
    }
    s_log_started = 1;
    s_log_len = 0;
}

void wos_log(const char *fmt, ...)
{
    va_list ap;
    int n;

    if (s_log_len > LOG_BUF_SIZE - 512)
        wos_log_flush();
    va_start(ap, fmt);
    n = vsnprintf(s_log_buf + s_log_len, LOG_BUF_SIZE - s_log_len - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((uint32_t)n > LOG_BUF_SIZE - s_log_len - 2)
        n = LOG_BUF_SIZE - s_log_len - 2;
    s_log_len += n;
    if (!s_log_len || s_log_buf[s_log_len - 1] != '\n')
        s_log_buf[s_log_len++] = '\n';
}

int wos_trace_budget;
volatile uint32_t wos_crumb[8] __attribute__((aligned(32)));

void wos_log_tick(void)
{
    uint64_t now = wos_now_us();

    if (now - s_log_last_us >= 2000000u) {
        s_log_last_us = now;
        wos_log_flush();
    }
}

/* ---- The program's drawer -------------------------------------------- */

static BPTR s_progdir_lock;
static BPTR s_old_dir;
static int  s_in_progdir;

void wos_enter_progdir(void)
{
    s_progdir_lock = Lock((STRPTR)"PROGDIR:", ACCESS_READ);
    if (s_progdir_lock) {
        s_old_dir = CurrentDir(s_progdir_lock);
        s_in_progdir = 1;
    }
}

void wos_leave_progdir(void)
{
    if (s_in_progdir) {
        CurrentDir(s_old_dir);
        UnLock(s_progdir_lock);
        s_in_progdir = 0;
    }
}

void wos_make_absolute(char *path, unsigned size)
{
    char buf[512];
    BPTR lock;

    if (!path[0] || strchr(path, ':'))
        return;
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (!lock)
        return;
    if (NameFromLock(lock, (STRPTR)buf, sizeof(buf))) {
        strncpy(path, buf, size - 1);
        path[size - 1] = 0;
    }
    UnLock(lock);
}

/* ---- The image requester -------------------------------------------- */

int wos_request_image(char *buf, unsigned size)
{
    struct FileRequester *fr;
    struct TagItem tags[5];
    int ok = 0;

    if (!AslBase)
        return 0;
    tags[0].ti_Tag = ASLFR_TitleText;     tags[0].ti_Data = (ULONG)"WiiStation: choose a PlayStation disc image";
    tags[1].ti_Tag = ASLFR_DoPatterns;    tags[1].ti_Data = TRUE;
    tags[2].ti_Tag = ASLFR_InitialPattern;
    tags[2].ti_Data = (ULONG)"#?.(cue|bin|iso|img|chd|pbp|ccd|mds|toc|cbn|exe)";
    tags[3].ti_Tag = ASLFR_RejectIcons;   tags[3].ti_Data = TRUE;
    tags[4].ti_Tag = TAG_DONE;            tags[4].ti_Data = 0;

    fr = (struct FileRequester *)AllocAslRequest(ASL_FileRequest, tags);
    if (!fr)
        return 0;
    if (AslRequest(fr, NULL) && fr->fr_File && fr->fr_File[0]) {
        strncpy(buf, (const char *)fr->fr_Drawer, size - 1);
        buf[size - 1] = 0;
        if (AddPart((STRPTR)buf, (STRPTR)fr->fr_File, size))
            ok = 1;
    }
    FreeAslRequest(fr);
    return ok;
}

/* ---- Libraries -------------------------------------------------------- */

int wos_sys_init(void)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase       = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    AslBase       = OpenLibrary("asl.library", 38);
    LowLevelBase  = OpenLibrary("lowlevel.library", 40);   /* optional: pads */

    wos_log("WiiStation WarpOS build %s (%s %s)", WOS_BUILD_STAMP, __DATE__, __TIME__);
#ifdef WOS_CRUMBS
    wos_log("[diag] crumbs at %p", (void *)wos_crumb);
#endif
    if (!IntuitionBase || !GfxBase) {
        wos_log("intuition.library or graphics.library v39 missing");
        return -1;
    }
    s_log_last_us = wos_now_us();
    return 0;
}

void wos_sys_shutdown(void)
{
    wos_log_flush();
    if (LowLevelBase)  CloseLibrary(LowLevelBase);
    if (AslBase)       CloseLibrary(AslBase);
    if (GfxBase)       CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    LowLevelBase = AslBase = NULL;
    GfxBase = NULL;
    IntuitionBase = NULL;
}
