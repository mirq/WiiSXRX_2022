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
#pragma pack(push, 2)
#include <dos/dosextens.h>
#pragma pack(pop)

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

uint32_t wos_prof_ticks[WOS_PROF_N];
uint32_t wos_prof_reads;

#define PC_SLOTS 1024
static struct { uint32_t pc, ticks; } s_pc_hist[PC_SLOTS];
static uint32_t s_pc_last, s_tb_prev;

void wos_pc_sample(uint32_t pc, uint32_t tb)
{
    uint32_t h, i;
    if (s_tb_prev && s_pc_last) {
        uint32_t d = tb - s_tb_prev;
        h = (s_pc_last >> 2) * 2654435761u >> 22;      /* 10 bits */
        for (i = 0; i < 8; i++, h = (h + 1) & (PC_SLOTS - 1)) {
            if (s_pc_hist[h].pc == s_pc_last || !s_pc_hist[h].pc) {
                s_pc_hist[h].pc = s_pc_last;
                s_pc_hist[h].ticks += d;
                break;
            }
        }
    }
    s_pc_last = pc;
    s_tb_prev = tb;
}

/* The eight pcs with the most time, as a share of all, then start again. */
void wos_pc_report(void)
{
    uint64_t total = 0;
    int top[8], n, i, j;
    char line[400];
    int len;

    for (i = 0; i < PC_SLOTS; i++)
        total += s_pc_hist[i].ticks;
    if (!total)
        return;
    for (n = 0; n < 8; n++) {
        int best = -1;
        for (i = 0; i < PC_SLOTS; i++) {
            for (j = 0; j < n; j++)
                if (top[j] == i)
                    break;
            if (j < n || !s_pc_hist[i].ticks)
                continue;
            if (best < 0 || s_pc_hist[i].ticks > s_pc_hist[best].ticks)
                best = i;
        }
        if (best < 0)
            break;
        top[n] = best;
    }
    len = snprintf(line, sizeof line, "[pcs]");
    for (i = 0; i < n && len < (int)sizeof line - 24; i++)
        len += snprintf(line + len, sizeof line - len, " %08lx %lu%%",
                        (unsigned long)s_pc_hist[top[i]].pc,
                        (unsigned long)((uint64_t)s_pc_hist[top[i]].ticks * 100 / total));
    wos_log("%s", line);
    memset(s_pc_hist, 0, sizeof s_pc_hist);
}

uint32_t wos_tb_hz(void)
{
    return (uint32_t)s_tb_hz;
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

/* ---- The 7410's performance counters (SetEnv WSX_PMC 1) --------------- */

int wos_pmc_on;
uint32_t wos_prim_cyc[256], wos_prim_n[256];
uint32_t wos_pmc_acc[WOS_PROF_N][4];
static uint32_t s_pmc_sel[4], s_pmc_t0[4];

#define MTSPR(n, v) __asm__ volatile("mtspr " #n ",%0" : : "r"(v))
#define MFSPR(n) ({ uint32_t _v; __asm__ volatile("mfspr %0," #n : "=r"(_v)); _v; })

/* MMCR0 (952) and MMCR1 (956) can be written only in supervisor mode; the
 * counters then run in both modes and are read through UPMC1-4. */
void wos_pmc_init(void)
{
    const char *sel = wos_getenv("WSX_PMCSEL");
    uint32_t v[4], u[4], old;
    static volatile uint32_t buf[64 * 1024];   /* 256 KB: a read every 32 bytes misses */
    int i;

    if (!wos_env_int("WSX_PMC", 0))
        return;
    s_pmc_sel[0] = 1; s_pmc_sel[1] = 5; s_pmc_sel[2] = 5; s_pmc_sel[3] = 2;
    if (sel && *sel)
        sscanf(sel, "%lu,%lu,%lu,%lu", (unsigned long *)&s_pmc_sel[0], (unsigned long *)&s_pmc_sel[1],
               (unsigned long *)&s_pmc_sel[2], (unsigned long *)&s_pmc_sel[3]);
    old = Super();
    MTSPR(952, 0x80000000u);            /* freeze while setting up */
    MTSPR(953, 0); MTSPR(954, 0); MTSPR(957, 0); MTSPR(958, 0);
    MTSPR(956, ((s_pmc_sel[2] & 31) << 27) | ((s_pmc_sel[3] & 31) << 22));
    MTSPR(952, ((s_pmc_sel[0] & 127) << 6) | (s_pmc_sel[1] & 63));
    v[0] = MFSPR(952); v[1] = MFSPR(956);
    User(old);
    wos_pmc_on = 1;
    wos_log("[pmc] events %lu,%lu,%lu,%lu (MMCR0 %08lx MMCR1 %08lx)",
            (unsigned long)s_pmc_sel[0], (unsigned long)s_pmc_sel[1], (unsigned long)s_pmc_sel[2],
            (unsigned long)s_pmc_sel[3], (unsigned long)v[0], (unsigned long)v[1]);
    /* a check of what the events count: 8192 reads 32 bytes apart, twice
     * (the second time the 256 KB are partly in the L2), and a busy loop */
    for (i = 0; i < 2; i++) {
        uint32_t j, sum = 0;
        wos_pmc_read(u);
        for (j = 0; j < 64 * 1024; j += 8)
            sum += buf[j];
        wos_pmc_read(v);
        wos_log("[pmc] 8192 strided reads: %lu %lu %lu %lu (%lu)",
                (unsigned long)(v[0] - u[0]), (unsigned long)(v[1] - u[1]),
                (unsigned long)(v[2] - u[2]), (unsigned long)(v[3] - u[3]), (unsigned long)sum);
    }
    {
        volatile uint32_t k = 0;
        uint32_t t0 = wos_tb_lo(), j;
        wos_pmc_read(u);
        for (j = 0; j < 100000; j++)
            k += j;
        wos_pmc_read(v);
        wos_log("[pmc] 100000-turn loop: %lu %lu %lu %lu in %lu time-base ticks (%lu Hz)",
                (unsigned long)(v[0] - u[0]), (unsigned long)(v[1] - u[1]),
                (unsigned long)(v[2] - u[2]), (unsigned long)(v[3] - u[3]),
                (unsigned long)(wos_tb_lo() - t0), (unsigned long)wos_tb_hz());
    }
    wos_pmc_read(s_pmc_t0);
}

/* Per frame: the whole, the stages the [prof] line takes out of the cpu
 * share, and what is left (translated code, branch tests, hardware). */
void wos_pmc_report(unsigned frames)
{
    static const struct { int st; const char *name; } part[] = {
        { WOS_PROF_GPU, "gpu" }, { WOS_PROF_RCNT, "rcnt" }, { WOS_PROF_IRQ, "irq" },
        { WOS_PROF_GTE, "gte" }, { WOS_PROF_HW, "hw" }, { WOS_PROF_BT, "bt" },
    };
    uint32_t now[4], tot[4], rest[4];
    char line[600];
    int len, i, k;

    if (!wos_pmc_on || !frames)
        return;
    wos_pmc_read(now);
    for (k = 0; k < 4; k++) {
        tot[k] = now[k] - s_pmc_t0[k];
        /* gte and hw run inside the cpu share; hw also holds the gpu */
        rest[k] = tot[k] - wos_pmc_acc[WOS_PROF_GPU][k] - wos_pmc_acc[WOS_PROF_RCNT][k]
                  - wos_pmc_acc[WOS_PROF_IRQ][k] - wos_pmc_acc[WOS_PROF_GTE][k]
                  - (wos_pmc_acc[WOS_PROF_HW][k] - wos_pmc_acc[WOS_PROF_GPU][k]);
    }
    len = snprintf(line, sizeof line, "[pmc] per frame (k): all %lu/%lu/%lu/%lu",
                   (unsigned long)(tot[0] / frames / 1000), (unsigned long)(tot[1] / frames / 1000),
                   (unsigned long)(tot[2] / frames / 1000), (unsigned long)(tot[3] / frames / 1000));
    for (i = 0; i < (int)(sizeof part / sizeof part[0]); i++) {
        const uint32_t *a = wos_pmc_acc[part[i].st];
        len += snprintf(line + len, sizeof line - len, ", %s %lu/%lu/%lu/%lu", part[i].name,
                        (unsigned long)(a[0] / frames / 1000), (unsigned long)(a[1] / frames / 1000),
                        (unsigned long)(a[2] / frames / 1000), (unsigned long)(a[3] / frames / 1000));
    }
    snprintf(line + len, sizeof line - len, ", rest %lu/%lu/%lu/%lu",
             (unsigned long)(rest[0] / frames / 1000), (unsigned long)(rest[1] / frames / 1000),
             (unsigned long)(rest[2] / frames / 1000), (unsigned long)(rest[3] / frames / 1000));
    wos_log("%s", line);
    /* the GPU commands with the most PMC1 events: count and average a frame */
    {
        int top[8], n;
        len = snprintf(line, sizeof line, "[pmc] gpu commands (k events, count, each):");
        for (n = 0; n < 8; n++) {
            int best = -1, j;
            for (i = 0; i < 256; i++) {
                for (j = 0; j < n; j++)
                    if (top[j] == i)
                        break;
                if (j < n || !wos_prim_n[i])
                    continue;
                if (best < 0 || wos_prim_cyc[i] > wos_prim_cyc[best])
                    best = i;
            }
            if (best < 0)
                break;
            top[n] = best;
            len += snprintf(line + len, sizeof line - len, " %02x %lu/%lu/%lu", best,
                            (unsigned long)(wos_prim_cyc[best] / frames / 1000),
                            (unsigned long)(wos_prim_n[best] / frames),
                            (unsigned long)(wos_prim_cyc[best] / wos_prim_n[best]));
        }
        wos_log("%s", line);
        memset(wos_prim_cyc, 0, sizeof wos_prim_cyc);
        memset(wos_prim_n, 0, sizeof wos_prim_n);
    }
    memset(wos_pmc_acc, 0, sizeof wos_pmc_acc);
    memcpy(s_pmc_t0, now, sizeof now);
}

/* ---- The log ---------------------------------------------------------- */

/* A log line per event written straight to disk cost OpenSWOS 60 ms a frame:
 * the lines are collected here and written out every 2 s and at exit. */
#define LOG_BUF_SIZE (64 * 1024)
#define LOG_PATH     "PROGDIR:wiistation.log"

static char     s_log_bufs[2][LOG_BUF_SIZE];
static char    *s_log_buf = s_log_bufs[0];
static int      s_log_cur;
static uint32_t s_log_len;
static uint64_t s_log_last_us;
static int      s_log_started;
static int      s_shutting_down;

/* The flush as a DOS packet the PPC does not wait for (as wos_cdasync's
 * reads): the file stays open (shared, so the bridge can read it), the
 * lines go out of the other buffer, and only the next flush waits for the
 * previous write, long done by then. An open, append and close every 2 s
 * stopped the PPC 20-28 ms, a frame lost in every 120. At the shutdown
 * and in the exit hooks the writes are synchronous again. SetEnv
 * WSX_LOGASYNC 0: fopen/fwrite/fclose every time, as before. */
static BPTR s_lfh;
static struct FileHandle *s_lfhp;
static struct MsgPort *s_lport;
static struct StandardPacket *s_lpkt;
static int s_lpend, s_log_async = -1;

static void log_wait(void)
{
    if (s_lpend) {
        WaitPort(s_lport);
        GetMsg(s_lport);
        s_lpend = 0;
    }
}

static void wos_log_close(void)
{
    log_wait();
    s_log_async = 0;
    if (s_lfh)
        Close(s_lfh);
    s_lfh = 0;
    s_lfhp = NULL;
    if (s_lport)
        DeleteMsgPort(s_lport);
    s_lport = NULL;
}

static int log_open_async(void)
{
    if (s_lfh)
        return 1;
    if (!s_log_started) {
        BPTR f = Open((CONST_STRPTR)LOG_PATH, MODE_NEWFILE);
        if (f)
            Close(f);
        s_log_started = 1;
    }
    s_lfh = Open((CONST_STRPTR)LOG_PATH, MODE_READWRITE);
    if (!s_lfh)
        return 0;
    Seek(s_lfh, 0, OFFSET_END);
    s_lfhp = (struct FileHandle *)BADDR(s_lfh);
    s_lport = CreateMsgPort();
    if (!s_lpkt)
        s_lpkt = wos_alloc_ppc(sizeof(*s_lpkt));
    if (!s_lfhp->fh_Type || !s_lport || !s_lpkt) {
        wos_log_close();
        return 0;
    }
    atexit(wos_log_close);
    return 1;
}

uint32_t wos_log_flushes, wos_log_flush_us;
void wos_log_flush(void)
{
    FILE *f;
    uint32_t t0;

    if (!s_log_len)
        return;
    wos_log_flushes++;
    t0 = wos_tb_lo();
    if (s_log_async < 0)
        s_log_async = wos_env_int("WSX_LOGASYNC", 1);
    if (s_log_async && !s_shutting_down && log_open_async()) {
        struct StandardPacket *p = s_lpkt;
        log_wait();
        wos_cache_flush_data(s_log_buf, s_log_len);
        memset(p, 0, sizeof(*p));
        p->sp_Msg.mn_Node.ln_Name = (char *)&p->sp_Pkt;
        p->sp_Msg.mn_ReplyPort = s_lport;
        p->sp_Pkt.dp_Link = &p->sp_Msg;
        p->sp_Pkt.dp_Port = s_lport;
        p->sp_Pkt.dp_Type = ACTION_WRITE;
        p->sp_Pkt.dp_Arg1 = s_lfhp->fh_Arg1;
        p->sp_Pkt.dp_Arg2 = (LONG)s_log_buf;
        p->sp_Pkt.dp_Arg3 = (LONG)s_log_len;
        wos_cache_flush_data(p, sizeof(*p));
        PutMsg(s_lfhp->fh_Type, &p->sp_Msg);
        s_lpend = 1;
        s_log_cur ^= 1;
        s_log_buf = s_log_bufs[s_log_cur];
        s_log_len = 0;
        wos_log_flush_us += (wos_tb_lo() - t0) / (wos_tb_hz() / 1000000u);
        return;
    }
    if (s_lfh) {
        log_wait();
        Write(s_lfh, s_log_buf, (LONG)s_log_len);
    } else {
        f = fopen(LOG_PATH, s_log_started ? "a" : "w");
        if (f) {
            fwrite(s_log_buf, 1, s_log_len, f);
            fclose(f);
        }
    }
    s_log_started = 1;
    s_log_len = 0;
    wos_log_flush_us += (wos_tb_lo() - t0) / (wos_tb_hz() / 1000000u);
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

/* ---- PPC exceptions ---------------------------------------------------- */

/* A data, instruction or program exception ends the PPC task without a
 * word in the log. The handler keeps the first one's context here, for
 * amiga_inspect_memory afterwards: [0] 0xe5c0 once set, [1] the exception
 * id, [2] srr0, [3] srr1, [4] dar, [5] dsisr, [6] lr, [7] ctr, [8..39] r0..r31.
 * Then WarpOS goes on as before. */
volatile uint32_t wos_exc[40] __attribute__((aligned(32)));
static APTR s_exc_handler;

static ULONG wos_exc_handler(struct EXCContext *ec)
{
    int i;
    if (wos_exc[0] != 0xe5c0) {
        wos_exc[1] = ec->ec_ExcID;
        wos_exc[2] = ec->ec_UPC.ec_SRR0;
        wos_exc[3] = ec->ec_SRR1;
        wos_exc[4] = ec->ec_DAR;
        wos_exc[5] = ec->ec_DSISR;
        wos_exc[6] = ec->ec_LR;
        wos_exc[7] = ec->ec_CTR;
        for (i = 0; i < 32; i++)
            wos_exc[8 + i] = ec->ec_GPR[i];
        wos_exc[0] = 0xe5c0;
        for (i = 0; i < 40; i += 8)
            __asm__ volatile("dcbst 0,%0" : : "r"(&wos_exc[i]) : "memory");
        __asm__ volatile("sync");
    }
    return EXCRETURN_NORMAL;
}

/* exit() from anywhere but our own shutdown would leave no trace */
static void wos_atexit(void)
{
    if (s_shutting_down)
        return;
    wos_log("[diag] exit() outside the shutdown: crumbs %lu frame %lu pc %08lx val %08lx",
            (unsigned long)wos_crumb[0], (unsigned long)wos_crumb[1],
            (unsigned long)wos_crumb[2], (unsigned long)wos_crumb[3]);
    {
        /* the return addresses up the stack (SysV back chain: [0] the
         * caller's frame, [1] its saved lr); wos_atexit's own address
         * gives the load offset for addr2line */
        uint32_t *fp = (uint32_t *)__builtin_frame_address(0);
        char line[300];
        int i, len = snprintf(line, sizeof line, "[diag] exit() stack (wos_log at %p):", (void *)wos_log);
        for (i = 0; i < 24 && fp && ((uint32_t)fp & 3) == 0 && len < (int)sizeof line - 12; i++) {
            uint32_t *next = (uint32_t *)fp[0];
            if (!next || next <= fp)
                break;
            len += snprintf(line + len, sizeof line - len, " %08lx", (unsigned long)next[1]);
            fp = next;
        }
        wos_log("%s", line);
    }
    wos_log_flush();
}

/* ---- A sampling profiler: SetEnv WSX_PMCPROF <cycles> ------------------
 * PMC1 counts cycles from 0x80000000 - period; when it turns negative the
 * performance monitor interrupt (MMCR0[PMXE], cleared by the 7410 when it
 * fires) gives the interrupted pc and lr to this handler, which stores them
 * and arms the next one. wos_pcprof_poll (every present) starts it at
 * present WSX_PMCPROF_AT (default 400) and writes T:pcsamp.bin when the
 * buffer is full: "PCS1", count, the run-time address of wos_log (for
 * addr2line), the dynarec buffer's start and size, then count x (pc, lr). */
#define PCS_MAX 65536
static uint32_t s_pcs[PCS_MAX][2];
static volatile uint32_t s_pcs_n;
static uint32_t s_pcs_period;
static volatile int s_pcs_state;        /* 0 off, 1 sampling, 2 full, 3 written */
static APTR s_pcs_handler;
extern char *wos_recmem_base;
extern uint32_t wos_recmem_size;

static ULONG wos_pcs_handler(struct EXCContext *ec)
{
    uint32_t n = s_pcs_n;
    if (s_pcs_state == 1) {
        if (n < PCS_MAX) {
            s_pcs[n][0] = ec->ec_UPC.ec_SRR0;
            s_pcs[n][1] = ec->ec_LR;
            s_pcs_n = n + 1;
        }
        if (n + 1 >= PCS_MAX)
            s_pcs_state = 2;
    }
    MTSPR(953, 0x80000000u - s_pcs_period);
    if (s_pcs_state == 1)
        MTSPR(952, MFSPR(952) | 0x04008000u);      /* PMXE | PMC1CE */
    else
        MTSPR(952, MFSPR(952) & ~0x04008000u);
    return EXCRETURN_ABORT;
}

void wos_pcprof_poll(unsigned presents)
{
    static int at = -2;
    if (at == -2) {
        s_pcs_period = (uint32_t)wos_env_int("WSX_PMCPROF", 0);
        at = s_pcs_period && wos_pmc_on ? wos_env_int("WSX_PMCPROF_AT", 400) : -1;
    }
    if (at < 0)
        return;
    if (s_pcs_state == 0 && presents >= (unsigned)at) {
        struct TagItem tags[] = {
            { EXCATTR_CODE, (ULONG)wos_pcs_handler },
            { EXCATTR_DATA, 0 },
            { EXCATTR_TASK, 0 },
            { EXCATTR_EXCID, EXCF_PERFMON },
            { EXCATTR_FLAGS, EXCF_GLOBAL | EXCF_LARGECONTEXT },
            { EXCATTR_NAME, (ULONG)"WiiStation prof" },
            { EXCATTR_PRI, 0 },
            { TAG_END, 0 }
        };
        uint32_t old;
        s_pcs_handler = SetExcHandler(tags);
        wos_log("[pcprof] sampling every %lu cycles from present %u%s", (unsigned long)s_pcs_period,
                presents, s_pcs_handler ? "" : " (no handler)");
        wos_log_flush();
        if (!s_pcs_handler) {
            at = -1;
            return;
        }
        s_pcs_state = 1;
        old = Super();
        MTSPR(953, 0x80000000u - s_pcs_period);
        MTSPR(952, MFSPR(952) | 0x04008000u);
        User(old);
    } else if (s_pcs_state == 2) {
        BPTR fh;
        uint32_t hdr[5];
        uint32_t old = Super();
        MTSPR(952, MFSPR(952) & ~0x04008000u);
        User(old);
        RemExcHandler(s_pcs_handler);
        s_pcs_state = 3;
        hdr[0] = 0x50435331; hdr[1] = s_pcs_n; hdr[2] = (uint32_t)wos_log;
        hdr[3] = (uint32_t)wos_recmem_base; hdr[4] = wos_recmem_size;
        fh = Open((CONST_STRPTR)"T:pcsamp.bin", MODE_NEWFILE);
        if (fh) {
            Write(fh, hdr, sizeof hdr);
            Write(fh, s_pcs, s_pcs_n * 8);
            {   /* then the blocks: count, count x (PSX pc, host code) */
                extern int wos_rec_blockmap(uint32_t *out, int max);
                uint32_t nb = (uint32_t)wos_rec_blockmap(&s_pcs[0][0], PCS_MAX);
                Write(fh, &nb, 4);
                Write(fh, s_pcs, nb * 8);
            }
            Close(fh);
        }
        wos_log("[pcprof] %lu samples written to T:pcsamp.bin (wos_log at %p, code buffer %p)",
                (unsigned long)s_pcs_n, (void *)wos_log, (void *)wos_recmem_base);
    }
}

static void wos_exc_install(void)
{
    atexit(wos_atexit);
    struct TagItem tags[] = {
        { EXCATTR_CODE, (ULONG)wos_exc_handler },
        { EXCATTR_DATA, 0 },
        { EXCATTR_TASK, 0 },
        { EXCATTR_EXCID, EXCF_MCHECK | EXCF_DACCESS | EXCF_IACCESS | EXCF_PROGRAM },
        { EXCATTR_FLAGS, EXCF_LOCAL | EXCF_LARGECONTEXT },
        { EXCATTR_NAME, (ULONG)"WiiStation" },
        { EXCATTR_PRI, 0 },
        { TAG_END, 0 }
    };
    s_exc_handler = SetExcHandler(tags);
    wos_log("[diag] exception record at %p%s", (void *)wos_exc, s_exc_handler ? "" : " (no handler)");
    if (wos_env_int("WSX_CRASHTEST", 0)) {
        wos_log("[diag] crash test: a store to 0xdeadbee0");
        wos_log_flush();
        *(volatile uint32_t *)0xdeadbee0 = 1;
        wos_log("[diag] crash test: still here, exception record %08lx at %08lx",
                (unsigned long)wos_exc[0], (unsigned long)wos_exc[2]);
    }
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
    wos_exc_install();
    if (!IntuitionBase || !GfxBase) {
        wos_log("intuition.library or graphics.library v39 missing");
        return -1;
    }
    s_log_last_us = wos_now_us();
    return 0;
}

void wos_sys_shutdown(void)
{
    s_shutting_down = 1;
    if (s_exc_handler) {
        RemExcHandler(s_exc_handler);
        s_exc_handler = NULL;
    }
    wos_log_flush();
    if (LowLevelBase)  CloseLibrary(LowLevelBase);
    if (AslBase)       CloseLibrary(AslBase);
    if (GfxBase)       CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    LowLevelBase = AslBase = NULL;
    GfxBase = NULL;
    IntuitionBase = NULL;
}
