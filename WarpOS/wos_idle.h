/*
 * wos_idle.h -- loops that only wait for an interrupt (see wos_idle.c).
 */
#ifndef WOS_IDLE_H
#define WOS_IDLE_H

#define WOS_IDLE_SLOTS 4

typedef struct {
    u32 pc;     /* the loop is entered here... */
    u32 pred;   /* ...from this block, by its backward branch at br */
    u32 br, op;
    u32 skips;
    u32 regs[34];   /* r0..r31, hi, lo at the last arrival */
} WosIdle;

extern WosIdle wos_idle[WOS_IDLE_SLOTS];
extern unsigned int wos_idle_found, wos_idle_skips;

void wos_idle_start_trace(void);
int  wos_idle_tracing(void);
void wos_idle_trace(u32 pc);
int  wos_idle_still(WosIdle *w);
int  wos_idle_same_round(const u32 *last, const u32 *now);

#endif
