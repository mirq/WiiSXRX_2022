/*
 * wos_vlc.c -- PsyQ's movie bitstream decoder, run as C.
 *
 * A PlayStation movie frame is Huffman-coded on the disc: the game's CPU
 * turns the bits into MDEC run/level codes (DecDCTvlc2 in libpress) and
 * the MDEC does the rest. That loop reads a few bits, looks them up, and
 * branches, so the dynarec turns it into thousands of tiny blocks a frame,
 * each with its exit bookkeeping: in Tekken 3's intro it is most of the
 * 17 ms of game code per frame.
 *
 * When the dynarec starts a block at a routine whose code is this decoder
 * (the addresses of its state words masked out), the block calls
 * wos_vlc_run instead: the same instructions transcribed into C, on the
 * same registers, tables and memory, ending as the routine's jr ra does.
 * It returns the number of instructions the routine executed, so the
 * game's clock moves as it would have. A call can take half a frame; it
 * stops at the top of its main loop when the next emulator event is due,
 * with the registers as the routine has them there, so a CD or vblank
 * interrupt is taken on time (a CD sector handled late is lost). The block
 * at that loop head is a call to the C as well.
 *
 * SetEnv WSX_VLC 0 leaves the routine to the dynarec; 2 checks every call:
 * a small MIPS interpreter runs the routine's own code and the C version
 * runs beside it, both into write logs; registers, writes and instruction
 * counts are compared, and the interpreter's result is the one kept.
 */
#include <stdlib.h>
#include <string.h>
#include "../psxcommon.h"
#include "../r3000a.h"
#include "../psxmem.h"
#include "wos_sys.h"

#define NWORDS 216

/* DecDCTvlc2 as Tekken 3 has it, with the lui/addiu pairs that load the
 * addresses of its state words cleared to their opcode halves. */
static const u32 s_ref[NWORDS] = {
    0x3c080000, 0x25080000, 0x20c60800, 0x3c010001, 0x00c13820, 0x1480000f,
    0x8d090000, 0x3c080000, 0x25080000, 0x8d040000, 0x8d050004, 0x8d020008,
    0x8d03000c, 0x8d0c0010, 0x8d0d0014, 0x8d0f0018, 0x8d18001c, 0x8d190020,
    0x01294820, 0x10000064, 0x00a97020, 0x00006820, 0x00007820, 0x0000c020,
    0x0000c820, 0x01294820, 0x00a97020, 0x8c890000, 0x948c0004, 0x948a0006,
    0x94820008, 0x9483000a, 0x214afffd, 0x05400002, 0x000c6280, 0x200d0001,
    0x2084000c, 0x00021400, 0x00431025, 0x00001825, 0xaca90000, 0x3129ffff,
    0x00094880, 0x25290004, 0x01254820, 0x3c080000, 0x25080000, 0xad090000,
    0x20a50002, 0x11a00035, 0x00024582, 0x390103ff, 0x10200085, 0x20a50002,
    0x21a1fffd, 0x04200002, 0x20c1fc00, 0x2021fc00, 0x00024602, 0x00084080,
    0x01014020, 0x95090000, 0x950a0002, 0x00004024, 0x1140000a, 0x01221004,
    0x20010020, 0x002a0822, 0x00224006, 0x04400004, 0x01421004, 0x200bffff,
    0x002b5806, 0x010b4022, 0x006a1820, 0x00691820, 0x30610010, 0x10200005,
    0x3063000f, 0x94890000, 0x20840002, 0x00694804, 0x00491025, 0x21a1fffe,
    0x1c200008, 0x03284820, 0x10200004, 0x03084820, 0x01e84820, 0x10000004,
    0x01e87820, 0x10000002, 0x0308c020, 0x0328c820, 0x00094880, 0x312903ff,
    0x01894825, 0x21ad0001, 0x21a1fff9, 0x14200011, 0xa4a90000, 0x1000000f,
    0x21adfffa, 0x390101ff, 0x10200051, 0x20a50002, 0x00021280, 0x2063000a,
    0x30610010, 0x10200005, 0x3063000f, 0x94890000, 0x20840002, 0x00694804,
    0x00491025, 0x01884025, 0xa4a80000, 0x00ae0823, 0x04210054, 0x20a50002,
    0x000244c2, 0x000840c0, 0x01064020, 0x8d090000, 0x00000000, 0x15200011,
    0x312100ff, 0x00021200, 0x20630008, 0x30610010, 0x10200005, 0x3063000f,
    0x94880000, 0x20840002, 0x00684004, 0x00481025, 0x000245c2, 0x00084080,
    0x01074020, 0x8d090000, 0x00005820, 0x10000002, 0x312100ff, 0x8d0b0004,
    0x00221004, 0x00611820, 0x30610010, 0x10200005, 0x3063000f, 0x94880000,
    0x20840002, 0x00684004, 0x00481025, 0x00094c02, 0x39217c1f, 0x10200015,
    0x3921fe00, 0x1020ff93, 0xa4a90000, 0x1160ffd8, 0x20a50002, 0x316affff,
    0x39417c1f, 0x1020000d, 0x3941fe00, 0x1020ff8b, 0xa4aa0000, 0x000b5402,
    0x1140ffcf, 0x20a50002, 0x39417c1f, 0x10200005, 0x3941fe00, 0x1020ff83,
    0xa4aa0000, 0x1000ffc8, 0x20a50002, 0x00024402, 0xa4a80000, 0x20a50002,
    0x94880000, 0x20840002, 0x00021400, 0x00684004, 0x1000ffbf, 0x00481025,
    0x3c080000, 0x25080000, 0x8d090000, 0x3408fe00, 0x00a90823, 0x04210004,
    0x00000000, 0xa4a80000, 0x1000fffb, 0x20a50002, 0x40096000, 0x00000000,
    0x3c010002, 0x01214825, 0x40896000, 0x03e00008, 0x00001020, 0x3c080000,
    0x25080000, 0xad040000, 0xad050004, 0xad020008, 0xad03000c, 0xad0c0010,
    0xad0d0014, 0xad0f0018, 0xad18001c, 0xad190020, 0x03e00008, 0x20020001,
};

/* lui/addiu pairs: the size word, the saved state (twice), the end pointer (twice) */
static const int s_pairs[5] = { 0, 7, 45, 186, 203 };

enum { R_AT = 1, R_V0, R_V1, R_A0, R_A1, R_A2, R_A3,
       R_T0, R_T1, R_T2, R_T3, R_T4, R_T5, R_T6, R_T7, R_T8 = 24, R_T9, R_RA = 31 };

typedef struct { u32 addr, val, size; } LogEntry;

typedef struct {
    u32 r[32];
    u32 n;                      /* instructions executed */
    u32 cap;                    /* when checking: give up after this many */
    u32 budget;                 /* stop at the loop head after this many */
    u32 resume_pc;              /* the loop head (0x40c0 in Tekken 3) */
    u32 exit_pc;                /* where it stopped; 0: it returned */
    u32 sr_set, sr_val;         /* the mtc0 at the end of a picture */
    LogEntry *log;              /* NULL: straight to memory */
    u32 nlog, maxlog;
    u32 wlog[64], nwlog;        /* where the logged words are, for reads */
    u32 lo16, hi16;             /* halfwords written directly */
    u32 w32[16], nw32;          /* words written directly */
} Ctx;

static u32 s_size_addr, s_state_addr, s_end_addr;

/* Routines found: where, and the addresses of their state words. */
#define MAXFOUND 8
static struct { u32 pc, size, state, end; } s_found[MAXFOUND];
static int s_nfound;
static int s_mode = -1;
unsigned int wos_vlc_calls, wos_vlc_bad, wos_vlc_instr;

static u32 code_at(u32 a)
{
    u8 *p = PSXM(a);
    return p ? LOAD_SWAP32p(p) : 0;
}

static int is_hw(u32 a)
{
    u32 t = a >> 16;
    return t == 0x1f80 || t == 0x9f80 || t == 0xbf80;
}

static u32 rd32(Ctx *c, u32 a)
{
    u8 *p;
    if (c->log) {
        int i;
        for (i = (int)c->nwlog - 1; i >= 0; i--)
            if (c->log[c->wlog[i]].addr == a)
                return c->log[c->wlog[i]].val;
    }
    if (is_hw(a) || !(p = psxMemRLUT[a >> 16]))
        return psxMemRead32(a);
    return LOAD_SWAP32p(p + (a & 0xffff));
}

static u32 rd16(Ctx *c, u32 a)
{
    u8 *p;
    if (is_hw(a) || !(p = psxMemRLUT[a >> 16]))
        return psxMemRead16(a);
    return LOAD_SWAP16p(p + (a & 0xffff));
}

static void put_log(Ctx *c, u32 a, u32 v, u32 size)
{
    if (c->nlog < c->maxlog) {
        c->log[c->nlog].addr = a;
        c->log[c->nlog].val = v;
        c->log[c->nlog].size = size;
        if (size == 4 && c->nwlog < 64)
            c->wlog[c->nwlog++] = c->nlog;
    }
    c->nlog++;
}

static void wr16(Ctx *c, u32 a, u32 v)
{
    u8 *p;
    v &= 0xffff;
    if (c->log) {
        put_log(c, a, v, 2);
        return;
    }
    if (is_hw(a) || !(p = psxMemWLUT[a >> 16])) {
        psxMemWrite16(a, v);
        return;
    }
    STORE_SWAP16p(p + (a & 0xffff), v);
    if (a < c->lo16) c->lo16 = a;
    if (a > c->hi16) c->hi16 = a;
}

static void wr32(Ctx *c, u32 a, u32 v)
{
    u8 *p;
    if (c->log) {
        put_log(c, a, v, 4);
        return;
    }
    if (is_hw(a) || !(p = psxMemWLUT[a >> 16]) || c->nw32 == 16) {
        psxMemWrite32(a, v);
        return;
    }
    STORE_SWAP32p(p + (a & 0xffff), v);
    c->w32[c->nw32++] = a;
}

/* The routine, instruction by instruction; c->n counts them, delay slots
 * included. Labels are the routine's addresses in Tekken 3. With resume it
 * starts at the loop head, from the registers there. */
static void vlc_fast(Ctx *c, int resume)
{
    u32 at, v0, v1, a0, a1, a2, a3, t0, t1, t2, t3, t4, t5, t6, t7, t8, t9;
    u32 n = 0;
    int cond;

    at = c->r[R_AT]; t0 = c->r[R_T0]; t1 = c->r[R_T1];
    v0 = c->r[R_V0]; v1 = c->r[R_V1]; a0 = c->r[R_A0]; a1 = c->r[R_A1];
    a2 = c->r[R_A2]; a3 = c->r[R_A3]; t2 = c->r[R_T2]; t3 = c->r[R_T3];
    t4 = c->r[R_T4]; t5 = c->r[R_T5]; t6 = c->r[R_T6]; t7 = c->r[R_T7];
    t8 = c->r[R_T8]; t9 = c->r[R_T9];
    if (resume)
        goto L40c0;

    t0 = s_size_addr;
    a2 += 0x800;
    at = 0x10000;
    a3 = a2 + at;
    cond = a0 != 0;
    t1 = rd32(c, t0);
    n += 7;
    if (!cond) {
        t0 = s_state_addr;
        a0 = rd32(c, t0);        a1 = rd32(c, t0 + 4);
        v0 = rd32(c, t0 + 8);    v1 = rd32(c, t0 + 0xc);
        t4 = rd32(c, t0 + 0x10); t5 = rd32(c, t0 + 0x14);
        t7 = rd32(c, t0 + 0x18); t8 = rd32(c, t0 + 0x1c);
        t9 = rd32(c, t0 + 0x20);
        t1 += t1;
        t6 = a1 + t1;
        n += 14;
        goto L40c0;
    }
    /* 3f34: a new picture */
    t5 = t7 = t8 = t9 = 0;
    t1 += t1;
    t6 = a1 + t1;
    t1 = rd32(c, a0);
    t4 = rd16(c, a0 + 4);
    t2 = rd16(c, a0 + 6);
    v0 = rd16(c, a0 + 8);
    v1 = rd16(c, a0 + 0xa);
    t2 -= 3;
    cond = (s32)t2 < 0;
    t4 <<= 10;
    n += 14;
    if (!cond) {
        t5 = 1;
        n += 1;
    }
    /* 3f70 */
    a0 += 0xc;
    v0 <<= 16;
    v0 |= v1;
    v1 = 0;
    wr32(c, a1, t1);
    t1 &= 0xffff;
    t1 <<= 2;
    t1 += 4;
    t1 += a1;
    t0 = s_end_addr;
    wr32(c, t0, t1);
    a1 += 2;
    n += 13;
L3fa4:
    cond = t5 == 0;
    t0 = v0 >> 22;
    n += 2;
    if (cond)
        goto L407c;
    /* 3fac: a DC coefficient */
    at = t0 ^ 0x3ff;
    a1 += 2;
    n += 3;
    if (at == 0)
        goto L41c8;
    at = t5 - 3;
    cond = (s32)at < 0;
    at = a2 - 0x400;
    n += 3;
    if (!cond) {
        at -= 0x400;
        n += 1;
    }
    /* 3fc8 */
    t0 = v0 >> 24;
    t0 <<= 2;
    t0 += at;
    t1 = rd16(c, t0);
    t2 = rd16(c, t0 + 2);
    t0 = 0;
    cond = t2 == 0;
    v0 <<= t1 & 31;
    n += 8;
    if (!cond) {
        at = 0x20 - t2;
        t0 = v0 >> (at & 31);
        cond = (s32)v0 < 0;
        v0 <<= t2 & 31;
        n += 5;
        if (!cond) {
            t3 = 0xffffffffu >> (at & 31);
            t0 -= t3;
            n += 3;
        }
        /* 4008 */
        v1 += t2;
        n += 1;
    }
    /* 400c */
    v1 += t1;
    at = v1 & 0x10;
    v1 &= 0xf;
    n += 4;
    if (at) {
        t1 = rd16(c, a0);
        a0 += 2;
        t1 <<= v1 & 31;
        v0 |= t1;
        n += 4;
    }
    /* 402c: add to the predictor of this component */
    at = t5 - 2;
    t1 = t9 + t0;
    n += 3;
    if ((s32)at > 0) {
        t9 += t0;               /* 4054 */
        n += 1;
    } else {
        t1 = t8 + t0;
        n += 2;
        if (at == 0) {
            t8 += t0;           /* 404c */
            n += 2;
        } else {
            t1 = t7 + t0;
            t7 += t0;
            n += 3;
        }
    }
    /* 4058 */
    t1 <<= 2;
    t1 &= 0x3ff;
    t1 |= t4;
    t5 += 1;
    at = t5 - 7;
    wr16(c, a1, t1);
    n += 7;
    if (at == 0) {
        t5 -= 6;
        n += 2;
    }
    goto L40b4;
L407c:
    at = t0 ^ 0x1ff;
    a1 += 2;
    n += 3;
    if (at == 0)
        goto L41c8;
    v0 <<= 10;
    v1 += 10;
    at = v1 & 0x10;
    v1 &= 0xf;
    n += 5;
    if (at) {
        t1 = rd16(c, a0);
        a0 += 2;
        t1 <<= v1 & 31;
        v0 |= t1;
        n += 4;
    }
    /* 40ac */
    t0 = t4 | t0;
    wr16(c, a1, t0);
    n += 2;
L40b4:
    at = a1 - t6;
    a1 += 2;
    n += 3;
    if ((s32)at >= 0)
        goto L420c;
L40c0:
    /* Time for an interrupt: stop here, as the CPU would take it here. */
    if (n >= c->budget) {
        c->exit_pc = c->resume_pc;
        goto out;
    }
    if (c->cap && n > c->cap) {
        n = 0;
        goto out;
    }
    /* an AC coefficient */
    t0 = v0 >> 19;
    t0 <<= 3;
    t0 += a2;
    t1 = rd32(c, t0);
    at = t1 & 0xff;
    n += 7;
    if (t1 != 0) {
        t3 = rd32(c, t0 + 4);   /* 411c */
        n += 1;
    } else {
        v0 <<= 8;
        v1 += 8;
        at = v1 & 0x10;
        v1 &= 0xf;
        n += 5;
        if (at) {
            t0 = rd16(c, a0);
            a0 += 2;
            t0 <<= v1 & 31;
            v0 |= t0;
            n += 4;
        }
        /* 4100 */
        t0 = v0 >> 23;
        t0 <<= 2;
        t0 += a3;
        t1 = rd32(c, t0);
        t3 = 0;
        at = t1 & 0xff;
        n += 7;
    }
    /* 4120 */
    v0 <<= at & 31;
    v1 += at;
    at = v1 & 0x10;
    v1 &= 0xf;
    n += 5;
    if (at) {
        t0 = rd16(c, a0);
        a0 += 2;
        t0 <<= v1 & 31;
        v0 |= t0;
        n += 4;
    }
    /* 4144: up to three codes from the table entry */
    t1 >>= 16;
    cond = (t1 ^ 0x7c1f) == 0;
    at = t1 ^ 0xfe00;
    n += 4;
    if (cond)
        goto L41a4;
    wr16(c, a1, t1);
    n += 2;
    if (at == 0)
        goto L3fa4;
    a1 += 2;
    n += 2;
    if (t3 == 0)
        goto L40c0;
    t2 = t3 & 0xffff;
    cond = (t2 ^ 0x7c1f) == 0;
    at = t2 ^ 0xfe00;
    n += 4;
    if (cond)
        goto L41a4;
    wr16(c, a1, t2);
    n += 2;
    if (at == 0)
        goto L3fa4;
    t2 = t3 >> 16;
    a1 += 2;
    n += 3;
    if (t2 == 0)
        goto L40c0;
    cond = (t2 ^ 0x7c1f) == 0;
    at = t2 ^ 0xfe00;
    n += 3;
    if (cond)
        goto L41a4;
    wr16(c, a1, t2);
    n += 2;
    if (at == 0)
        goto L3fa4;
    a1 += 2;
    n += 2;
    goto L40c0;
L41a4:
    /* escape: the next 16 bits are the code */
    t0 = v0 >> 16;
    wr16(c, a1, t0);
    a1 += 2;
    t0 = rd16(c, a0);
    a0 += 2;
    v0 <<= 16;
    t0 <<= v1 & 31;
    v0 |= t0;
    n += 9;
    goto L40c0;
L41c8:
    /* end of the picture: pad to the end pointer, then return 0 */
    t0 = s_end_addr;
    t1 = rd32(c, t0);
    t0 = 0xfe00;
    n += 4;
    for (;;) {
        at = a1 - t1;
        n += 3;
        if ((s32)at >= 0)
            break;
        wr16(c, a1, t0);
        a1 += 2;
        n += 3;
    }
    /* 41f0 */
    t1 = psxRegs.CP0.n.SR;
    at = 0x20000;
    t1 |= at;
    c->sr_set = 1;
    c->sr_val = t1;
    v0 = 0;
    n += 7;
    goto out;
L420c:
    /* the buffer is full: save the state, return 1 */
    t0 = s_state_addr;
    wr32(c, t0, a0);        wr32(c, t0 + 4, a1);
    wr32(c, t0 + 8, v0);    wr32(c, t0 + 0xc, v1);
    wr32(c, t0 + 0x10, t4); wr32(c, t0 + 0x14, t5);
    wr32(c, t0 + 0x18, t7); wr32(c, t0 + 0x1c, t8);
    wr32(c, t0 + 0x20, t9);
    v0 = 1;
    n += 13;
out:
    c->r[R_AT] = at; c->r[R_V0] = v0; c->r[R_V1] = v1;
    c->r[R_A0] = a0; c->r[R_A1] = a1; c->r[R_A2] = a2; c->r[R_A3] = a3;
    c->r[R_T0] = t0; c->r[R_T1] = t1; c->r[R_T2] = t2; c->r[R_T3] = t3;
    c->r[R_T4] = t4; c->r[R_T5] = t5; c->r[R_T6] = t6; c->r[R_T7] = t7;
    c->r[R_T8] = t8; c->r[R_T9] = t9;
    c->n = n;
}

/* The check: the routine's own instructions, interpreted. Only what the
 * routine uses; anything else stops it (c->n = 0). The code keeps the
 * R3000's load delays, so loads take effect at once here. */
static int step(Ctx *c, u32 pc, u32 *target, int *taken)
{
    u32 *r = c->r, op = code_at(pc);
    u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
    u32 imm = op & 0xffff, simm = (u32)(s32)(s16)imm, b = pc + 4 + (simm << 2);
    int branch = 0;

    switch (op >> 26) {
    case 0x00:
        switch (op & 63) {
        case 0x00: r[rd] = r[rt] << sa; break;
        case 0x02: r[rd] = r[rt] >> sa; break;
        case 0x04: r[rd] = r[rt] << (r[rs] & 31); break;
        case 0x06: r[rd] = r[rt] >> (r[rs] & 31); break;
        case 0x08: branch = 1; *taken = 1; *target = r[rs]; break;
        case 0x20: case 0x21: r[rd] = r[rs] + r[rt]; break;
        case 0x22: case 0x23: r[rd] = r[rs] - r[rt]; break;
        case 0x24: r[rd] = r[rs] & r[rt]; break;
        case 0x25: r[rd] = r[rs] | r[rt]; break;
        default: return -1;
        }
        break;
    case 0x01:
        if (rt > 1)
            return -1;
        branch = 1; *target = b;
        *taken = rt ? (s32)r[rs] >= 0 : (s32)r[rs] < 0;
        break;
    case 0x04: branch = 1; *target = b; *taken = r[rs] == r[rt]; break;
    case 0x05: branch = 1; *target = b; *taken = r[rs] != r[rt]; break;
    case 0x07: branch = 1; *target = b; *taken = (s32)r[rs] > 0; break;
    case 0x08: case 0x09: r[rt] = r[rs] + simm; break;
    case 0x0c: r[rt] = r[rs] & imm; break;
    case 0x0d: r[rt] = r[rs] | imm; break;
    case 0x0e: r[rt] = r[rs] ^ imm; break;
    case 0x0f: r[rt] = imm << 16; break;
    case 0x10:
        if (rs == 0 && rd == 12) r[rt] = psxRegs.CP0.n.SR;
        else if (rs == 4 && rd == 12) { c->sr_set = 1; c->sr_val = r[rt]; }
        else return -1;
        break;
    case 0x23: r[rt] = rd32(c, r[rs] + simm); break;
    case 0x25: r[rt] = rd16(c, r[rs] + simm); break;
    case 0x29: wr16(c, r[rs] + simm, r[rt]); break;
    case 0x2b: wr32(c, r[rs] + simm, r[rt]); break;
    default: return -1;
    }
    r[0] = 0;
    return branch;
}

static void vlc_interp(Ctx *c, u32 pc)
{
    u32 ra = c->r[R_RA], n = 0, target, t2;
    int taken, taken2;

    while (pc != ra && n < 50000000) {
        int k;
        if (pc == c->resume_pc && n >= c->budget) {
            c->exit_pc = pc;
            break;
        }
        k = step(c, pc, &target, &taken);
        n++;
        if (k < 0)
            break;
        if (!k) {
            pc += 4;
            continue;
        }
        k = step(c, pc + 4, &t2, &taken2);  /* the delay slot */
        n++;
        if (k != 0)
            break;
        pc = taken ? target : pc + 8;
    }
    c->n = pc == ra || c->exit_pc ? n : 0;
}

static u32 pair_addr(const u32 *w, int i)
{
    return ((w[i] & 0xffff) << 16) + (u32)(s32)(s16)(w[i + 1] & 0xffff);
}

/* Whether the code at pc is the decoder: the dynarec asks when it starts a
 * block. */
#define RESUME_OFF (0x40c0 - 0x3ee0)

static int match_entry(u32 pc);

int wos_vlc_match(u32 pc)
{
    int k;

    if (s_mode < 0)
        s_mode = wos_env_int("WSX_VLC", 1);
    if (!s_mode)
        return 0;
    /* the loop head of a routine found before, where a call that stopped
     * for an interrupt goes on */
    for (k = 0; k < s_nfound; k++)
        if (pc == s_found[k].pc + RESUME_OFF)
            return match_entry(s_found[k].pc);
    return match_entry(pc);
}

static int match_entry(u32 pc)
{
    u32 w[NWORDS];
    int i, j, k;

    if ((pc & 3) || !PSXM(pc) || !PSXM(pc + 4 * NWORDS - 4))
        return 0;
    if (code_at(pc) >> 16 != 0x3c08)
        return 0;
    for (i = 0; i < NWORDS; i++) {
        u32 m = ~0u;
        w[i] = code_at(pc + 4 * i);
        for (j = 0; j < 5; j++)
            if (i == s_pairs[j] || i == s_pairs[j] + 1)
                m = 0xffff0000;
        if ((w[i] & m) != s_ref[i])
            return 0;
    }
    if (pair_addr(w, s_pairs[1]) != pair_addr(w, s_pairs[4]) ||
        pair_addr(w, s_pairs[2]) != pair_addr(w, s_pairs[3]))
        return 0;
    for (k = 0; k < s_nfound && s_found[k].pc != pc; k++)
        ;
    if (k == MAXFOUND)
        k = MAXFOUND - 1;
    if (k == s_nfound) {
        s_nfound = k + 1;
        wos_log("[vlc] DecDCTvlc2 at %08lx (state %08lx), run as C%s",
                (unsigned long)pc, (unsigned long)pair_addr(w, s_pairs[1]),
                s_mode == 2 ? ", checked against its code" : "");
    }
    s_found[k].pc = pc;
    s_found[k].size = pair_addr(w, s_pairs[0]);
    s_found[k].state = pair_addr(w, s_pairs[1]);
    s_found[k].end = pair_addr(w, s_pairs[2]);
    return 1;
}

static LogEntry *s_log_a, *s_log_b;
#define MAXLOG (1 << 18)

static int same(const Ctx *a, const Ctx *b)
{
    return a->n && a->n == b->n && !memcmp(a->r, b->r, sizeof a->r) && a->nwlog < 64 &&
           a->nlog == b->nlog && a->nlog <= MAXLOG &&
           !memcmp(a->log, b->log, a->nlog * sizeof(LogEntry)) &&
           a->sr_set == b->sr_set && (!a->sr_set || a->sr_val == b->sr_val) &&
           a->exit_pc == b->exit_pc;
}

static void report(const Ctx *in, const Ctx *a, const Ctx *b)
{
    u32 i;
    wos_log("[vlc] call %u differs: %lu/%lu instructions, %lu/%lu writes, v0 %08lx/%08lx, a0 %08lx/%08lx, a1 %08lx/%08lx (in: a0 %08lx a1 %08lx a2 %08lx)",
            wos_vlc_calls, (unsigned long)a->n, (unsigned long)b->n,
            (unsigned long)a->nlog, (unsigned long)b->nlog,
            (unsigned long)a->r[R_V0], (unsigned long)b->r[R_V0],
            (unsigned long)a->r[R_A0], (unsigned long)b->r[R_A0],
            (unsigned long)a->r[R_A1], (unsigned long)b->r[R_A1],
            (unsigned long)in->r[R_A0], (unsigned long)in->r[R_A1], (unsigned long)in->r[R_A2]);
    for (i = 0; i < 32; i++)
        if (a->r[i] != b->r[i])
            wos_log("[vlc]   r%lu %08lx/%08lx", (unsigned long)i, (unsigned long)a->r[i], (unsigned long)b->r[i]);
    for (i = 0; i < a->nlog && i < b->nlog && i < MAXLOG; i++)
        if (memcmp(&a->log[i], &b->log[i], sizeof(LogEntry))) {
            wos_log("[vlc]   write %lu: %08lx=%lx/%lu, %08lx=%lx/%lu", (unsigned long)i,
                    (unsigned long)a->log[i].addr, (unsigned long)a->log[i].val, (unsigned long)a->log[i].size,
                    (unsigned long)b->log[i].addr, (unsigned long)b->log[i].val, (unsigned long)b->log[i].size);
            break;
        }
}

void MTC0(int reg, u32 val);

/* The block the dynarec made for the routine's entry calls this. Returns
 * the instructions the routine took. */
u32 wos_vlc_run(u32 budget)
{
    Ctx c;
    u32 pc = psxRegs.pc, i;
    int k, resume = 0;

    for (k = 0; k < s_nfound && s_found[k].pc != pc; k++)
        ;
    if (k == s_nfound) {
        for (k = 0; k < s_nfound && s_found[k].pc + RESUME_OFF != pc; k++)
            ;
        resume = 1;
        if (k == s_nfound)
            k = 0;              /* cannot happen: the block was made for a match */
    }
    s_size_addr = s_found[k].size;
    s_state_addr = s_found[k].state;
    s_end_addr = s_found[k].end;

    memset(&c, 0, sizeof c);
    memcpy(c.r, psxRegs.GPR.r, sizeof c.r);
    c.lo16 = ~0u;
    c.budget = budget ? budget : 1;
    c.resume_pc = s_found[k].pc + RESUME_OFF;
    if (!resume)
        wos_vlc_calls++;
    WOS_CRUMB_SET(4, wos_vlc_calls);
    WOS_CRUMB2(90, pc);

    if (s_mode == 2 && (s_log_a || ((s_log_a = malloc(MAXLOG * sizeof(LogEntry))) &&
                                    (s_log_b = malloc(MAXLOG * sizeof(LogEntry)))))) {
        static Ctx a, b;
        a = c; b = c;
        a.log = s_log_a; a.maxlog = MAXLOG;
        b.log = s_log_b; b.maxlog = MAXLOG;
        vlc_interp(&a, resume ? c.resume_pc : pc);
        WOS_CRUMB2(91, a.n);
        b.cap = a.n ? a.n + 64 : 60000000;
        vlc_fast(&b, resume);
        WOS_CRUMB2(92, b.n);
        if (!same(&a, &b)) {
            if (wos_vlc_bad < 8)
                report(&c, &a, &b);
            wos_vlc_bad++;
        }
        if (a.n && a.nlog <= MAXLOG) {
            for (i = 0; i < a.nlog; i++) {
                if (a.log[i].size == 4)
                    psxMemWrite32(a.log[i].addr, a.log[i].val);
                else
                    psxMemWrite16(a.log[i].addr, a.log[i].val);
            }
            memcpy(c.r, a.r, sizeof c.r);
            c.n = a.n;
            c.exit_pc = a.exit_pc;
            c.sr_set = a.sr_set;
            c.sr_val = a.sr_val;
        } else {
            vlc_fast(&c, resume);   /* the interpreter gave up: trust the C */
        }
    } else {
        vlc_fast(&c, resume);
    }

    WOS_CRUMB2(93, c.n);
    memcpy(psxRegs.GPR.r, c.r, sizeof c.r);
    psxRegs.pc = c.exit_pc ? c.exit_pc : c.r[R_RA];
    if (c.hi16 >= c.lo16)
        psxCpu->Clear(c.lo16 & ~3u, ((c.hi16 & ~3u) - (c.lo16 & ~3u)) / 4 + 1);
    for (i = 0; i < c.nw32; i++)
        psxCpu->Clear(c.w32[i] & ~3u, 1);
    WOS_CRUMB2(94, c.sr_set ? c.sr_val : 0);
    if (c.sr_set)
        MTC0(12, c.sr_val);
    wos_vlc_instr += c.n;
    WOS_CRUMB2(95, psxRegs.pc);
    return c.n;
}

/* PsyQ's StGetNext, by its code: the movie player polls it for the next
 * frame, and on the PSX nothing changes the frame ring but an interrupt.
 * A poll that will fail, made again right after the one before from the
 * same caller, lets the time run on to the next event: the rounds between
 * would only fail the same way (they read the pad and VSync(-1) as well,
 * which also change only at events). */
static const u32 s_stnext_ref[] = {
    0x00803821, 0x3c020000, 0x8c420000, 0x3c030000, 0x8c630000, 0x00021140,
    0x00623021, 0x94c30000, 0x24020001, 0x1462000d, 0x00a04021, 0x3c020000,
    0x8c420000, 0x3c010000, 0x10400002, 0xac200000, 0xa4c00000, 0x3c020000,
    0x8c420000, 0x3c030000, 0x8c630000, 0x00021140, 0x00623021, 0x94c30000,
    0x24020002, 0x14620012, 0x24020001, 0x24020004, 0xa4c20000, 0x00001021,
    0x3c030000, 0x8c630000, 0x3c040000, 0x8c840000, 0x3c050000, 0x8ca50000,
    0x00031940, 0x00832021, 0x00051980, 0x00651823, 0x00031940, 0x00832021,
    0xace40000, 0xad060000, 0x03e00008, 0x00000000,
};
#define ST_NWORDS (sizeof s_stnext_ref / sizeof s_stnext_ref[0])

static u32 s_st_pc, s_st_index, s_st_ring;
static u32 s_st_ra, s_st_cycle;
static int s_st_failed;
unsigned int wos_st_polls, wos_st_skips;

int wos_stnext_match(u32 pc)
{
    u32 w[ST_NWORDS];
    unsigned i;

    if ((pc & 3) || !PSXM(pc) || !PSXM(pc + 4 * ST_NWORDS - 4) || code_at(pc) != s_stnext_ref[0])
        return 0;
    for (i = 0; i < ST_NWORDS; i++) {
        u32 op = s_stnext_ref[i] >> 26;
        /* the addresses of its globals differ from game to game */
        u32 m = (op == 0x0f || op == 0x23 || op == 0x2b) ? 0xffff0000 : ~0u;
        w[i] = code_at(pc + 4 * i);
        if ((w[i] & m) != s_stnext_ref[i])
            return 0;
    }
    s_st_pc = pc;
    s_st_index = pair_addr(w, 1);
    s_st_ring = pair_addr(w, 3);
    s_st_failed = 0;
    return 1;
}

extern u32 wos_next_event;
extern int wos_noidle;

/* The block at StGetNext's entry calls this first. */
void wos_stnext_poll(void)
{
    u8 *pi = PSXM(s_st_index), *pr = PSXM(s_st_ring), *ps;
    u32 slot, ra = psxRegs.GPR.r[31], c = psxRegs.cycle;
    int state;

    wos_st_polls++;
    if (!pi || !pr)
        return;
    slot = LOAD_SWAP32p(pr) + (LOAD_SWAP32p(pi) << 5);
    ps = PSXM(slot);
    if (!ps)
        return;
    state = ps[0] | ps[1] << 8;
    if (state == 1 || state == 2) {
        s_st_failed = 0;            /* a frame, or the ring wraps: real work */
        return;
    }
    if (s_st_failed && ra == s_st_ra && c - s_st_cycle < 8192 && !wos_noidle &&
        (s32)(wos_next_event - c) > 0) {
        psxRegs.cycle = c = wos_next_event;
        wos_st_skips++;
    }
    s_st_failed = 1;
    s_st_ra = ra;
    s_st_cycle = c;
}
