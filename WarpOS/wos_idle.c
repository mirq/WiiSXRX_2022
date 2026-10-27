/*
 * wos_idle.c -- loops that only wait for an interrupt.
 *
 * Games spin until the vblank or the CD interrupt changes a word in RAM:
 * PsyQ's VSync() loops on its counter, Silent Hill's movie player asks
 * "is a frame decoded yet?" up to 2000 times. Emulating that spin costs the
 * PPC as much as real work. When the blocks of such a loop do nothing but
 * load, compute in registers, write the stack and branch, nothing can change
 * until the next event, so psxBranchTest moves the clock straight there.
 *
 * Finding one: every 256 branch tests the next 16 block pcs are traced. If
 * they repeat with a period of 8 or less, each block of the cycle (from its
 * first instruction to the branch that ends it, and its delay slot) is
 * checked. The loop is entered at a backward branch inside it; it is skipped
 * only when that branch is the one just taken. A loop that reads hardware
 * (GPU status, a timer) during the trace is left alone: that state moves
 * with time, not at events, and skipping to the next event costs frames.
 */
#include "../psxcommon.h"
#include "../r3000a.h"
#include "../psxmem.h"
#include "wos_idle.h"

#define TRACE 16
#define MAXP  8
#define MAXI  48            /* instructions in one block */

WosIdle wos_idle[WOS_IDLE_SLOTS];
static int s_slot;
static u32 s_trace[TRACE];
static int s_trace_n;
static unsigned int s_trace_hw;
extern unsigned int wos_hw_reads;
unsigned int wos_idle_found, wos_idle_skips;

static int fetch(u32 a, u32 *op)
{
    u8 *p = PSXM(a);
    if (!p)
        return 0;
    *op = LOAD_SWAP32p(p);
    return 1;
}

/* Whether one instruction leaves everything but registers and the stack as
 * it was. Branches and jumps are handled by the caller. */
static int harmless(u32 op)
{
    u32 o = op >> 26, rs = (op >> 21) & 31, f = op & 63;
    switch (o) {
    case 0x00:              /* SPECIAL */
        switch (f) {
        case 0x00: case 0x02: case 0x03: case 0x04: case 0x06: case 0x07:
        case 0x10: case 0x11: case 0x12: case 0x13:          /* mfhi.. mtlo */
        case 0x18: case 0x19: case 0x1a: case 0x1b:          /* mult, div */
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26: case 0x27: case 0x2a: case 0x2b:
            return 1;
        }
        return 0;
    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x0c: case 0x0d: case 0x0e: case 0x0f:              /* immediates */
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26:                         /* loads */
        return 1;
    case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2e:   /* stores */
        return rs == 29;    /* only into the stack */
    case 0x10:              /* COP0: mfc0 only */
        return rs == 0;
    }
    return 0;
}

/* Checks the block from start up to its last instruction; the pcs it may go
 * on to are returned: two for a conditional branch, one for j/jal, none
 * (meaning "any") for jr. *br is the branch's address. Returns 0 if the
 * block has another effect. */
static int check_block(u32 start, u32 next, u32 *br, u32 *sum)
{
    u32 a = start, op;
    int i;
    for (i = 0; i < MAXI; i++, a += 4) {
        u32 o, rs, rt, f, t1 = 0, t2 = 0, ds;
        int any = 0;
        if (!fetch(a, &op))
            return 0;
        *sum = *sum * 31 + op;
        o = op >> 26; rs = (op >> 21) & 31; rt = (op >> 16) & 31; f = op & 63;
        if (o == 0x04 || o == 0x05 || o == 0x06 || o == 0x07 || o == 0x01) {
            if (o == 0x01 && rt != 0x00 && rt != 0x01 && rt != 0x10 && rt != 0x11)
                return 0;
            t1 = a + 4 + ((s32)(s16)(op & 0xffff) << 2);
            t2 = a + 8;
        } else if (o == 0x02 || o == 0x03) {
            t1 = t2 = ((a + 4) & 0xf0000000) | ((op & 0x03ffffff) << 2);
        } else if (o == 0x00 && (f == 0x08 || f == 0x09)) {
            any = 1;
        } else {
            if (!harmless(op))
                return 0;
            continue;
        }
        (void)rs;
        if (!fetch(a + 4, &ds) || !harmless(ds))
            return 0;
        *sum = *sum * 31 + ds;
        *br = a;
        return any || next == t1 || next == t2;
    }
    return 0;
}

static void analyse(void)
{
    int p, i, k;
    u32 br[MAXP], sum = 0;

    for (p = 1; p <= MAXP; p++) {
        for (i = 0; i + p < TRACE; i++)
            if (s_trace[i] != s_trace[i + p])
                break;
        if (i + p == TRACE)
            break;
    }
    if (p > MAXP)
        return;
    for (i = 0; i < p; i++)
        if (!check_block(s_trace[i], s_trace[(i + 1) % p], &br[i], &sum))
            return;
    /* Enter at a block that a backward branch of the cycle leads to. */
    for (k = 0; k < p; k++) {
        int prev = (k + p - 1) % p;
        u32 op;
        if (!fetch(br[prev], &op))
            return;
        if ((op >> 26) >= 0x01 && (op >> 26) <= 0x07 && (op >> 26) != 0x02 && (op >> 26) != 0x03 &&
            s_trace[k] <= br[prev] &&
            s_trace[k] == br[prev] + 4 + ((s32)(s16)(op & 0xffff) << 2)) {
            WosIdle *w;
            int j;
            for (j = 0; j < WOS_IDLE_SLOTS; j++)
                if (wos_idle[j].pc == s_trace[k] && wos_idle[j].pred == s_trace[prev])
                    return;
            w = &wos_idle[s_slot];
            s_slot = (s_slot + 1) % WOS_IDLE_SLOTS;
            w->pc = s_trace[k];
            w->pred = s_trace[prev];
            w->br = br[prev];
            w->op = op;
            w->skips = 0;
            wos_idle_found++;
            return;
        }
    }
}

void wos_idle_trace(u32 pc)
{
    if (s_trace_n < TRACE) {
        s_trace[s_trace_n++] = pc;
        /* A loop that reads hardware waits on the GPU, a timer or the CD:
         * those move with time, not at events, so it is not skipped. */
        if (s_trace_n == TRACE && wos_hw_reads == s_trace_hw)
            analyse();
    }
}

void wos_idle_start_trace(void)
{
    s_trace_n = 0;
    s_trace_hw = wos_hw_reads;
}

int wos_idle_tracing(void)
{
    return s_trace_n < TRACE;
}

/* The loop's backward branch is still the instruction we checked. */
int wos_idle_still(WosIdle *w)
{
    u32 op;
    if (fetch(w->br, &op) && op == w->op)
        return 1;
    w->pc = 0;
    return 0;
}

/* Whether one round of an idle loop left the registers as the last one did,
 * but for one timeout counted down by one. */
int wos_idle_same_round(const u32 *last, const u32 *now)
{
    int i, diff = -1;
    for (i = 0; i < 34; i++) {
        if (last[i] != now[i]) {
            if (diff >= 0)
                return 0;
            diff = i;
        }
    }
    return diff < 0 || (now[diff] == last[diff] - 1 && now[diff] > 16);
}
