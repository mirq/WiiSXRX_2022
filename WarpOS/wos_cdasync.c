/*
 * wos_cdasync.c -- disc image reads that run while the emulator does.
 *
 * The 68k reads the image at about 1 MB/s, so a 64-sector run took ~160 ms
 * and the PPC stood still for all of it. Here a run is sent to the file
 * system as an ACTION_READ packet and the PPC goes on: while the game uses
 * one buffer, the next run is read into the other. The PPC only waits
 * (WaitPort) when the game asks for a sector that has not arrived yet.
 *
 * The handle is our own, so nothing else moves its file position.
 */
#include <string.h>

#include "wos_sys.h"
#include "wos_amiga.h"
#include "wos_cdasync.h"

#pragma pack(push, 2)
#include <dos/dosextens.h>
#pragma pack(pop)

#define RUN_SECTORS 64
/* A read the game waits for (a jump to a sector neither buffer holds):
 * this many sectors, and the rest of the run behind it as the read-ahead.
 * A whole run there stopped the PPC ~150 ms (XA music interleaved with game
 * data in Tekken 3's fights: 0.3-0.5 ms a frame on average). */
#define JUMP_SECTORS 4
/* The read-ahead after a jump doubles from there (4, 8, 16, 32), so each
 * run arrives before the game is through the one before it: a 64-sector
 * run straight after 8 sectors took the 68k ~150 ms while the game needed
 * its first sector ~50 ms later, and the PPC waited the rest (Tekken 3
 * fights: frames of 15-60 ms). In a steady stream every run has arrived
 * long before it is needed; what the PPC waits for is collecting it, the
 * 68k calls (GetMsg ~1 ms, WaitPort 2-5 ms with the message already in
 * the port), so the runs stay at READ_MAX and WaitPort is skipped when the
 * answer is in (arrived). SetEnv WSX_CDRUN n: the largest run. */
#define READ_MAX 32
static int s_read_max = -1;
#define SECTOR      2352
#define RUN_BYTES   (RUN_SECTORS * SECTOR)

static BPTR s_fh;
static struct FileHandle *s_fhp;
static struct MsgPort *s_reply;
static struct StandardPacket *s_pkt;
static unsigned char *s_buf[2];
static int s_first[2], s_count[2];  /* sectors held; count 0 = empty */
static int s_cur;                   /* the buffer being served */
static int s_pending = -1;          /* the buffer being filled, or -1 */
static int s_pend_first;
static int s_pend_len;              /* bytes asked for */
static int s_req[2];                /* sectors asked for, per buffer */
static uint32_t s_pos;              /* the file position after the last read */
static int s_ok;
static uint32_t s_send_tb;
/* [prof]: the age of a read when the game needed it (time-base ticks),
 * summed and the largest, for the waits */
uint32_t wos_cda_age_tb, wos_cda_age_max, wos_cda_lat_tb, wos_cda_lat_max;

static void send(LONG type, LONG a2, LONG a3)
{
    struct StandardPacket *p = s_pkt;
    memset(p, 0, sizeof(*p));
    p->sp_Msg.mn_Node.ln_Name = (char *)&p->sp_Pkt;
    p->sp_Msg.mn_ReplyPort = s_reply;
    p->sp_Pkt.dp_Link = &p->sp_Msg;
    p->sp_Pkt.dp_Port = s_reply;
    p->sp_Pkt.dp_Type = type;
    p->sp_Pkt.dp_Arg1 = s_fhp->fh_Arg1;
    p->sp_Pkt.dp_Arg2 = a2;
    p->sp_Pkt.dp_Arg3 = a3;
    p->sp_Pkt.dp_Res1 = 0x7ffffff1;     /* RES_PENDING */
    wos_cache_flush_data(p, sizeof(*p));
    PutMsg(s_fhp->fh_Type, &p->sp_Msg);
}

/* Whether the packet in flight has been answered, without a 68k call: the
 * file system sets dp_Res1 (sent as RES_PENDING) before it replies. Only
 * our own packet is read: a cache operation on the 68k's reply port
 * (exec memory) hangs this machine. */
#define RES_PENDING 0x7ffffff1
static int arrived(void)
{
    wos_cache_invalidate_data(s_pkt, sizeof(*s_pkt));
    return s_pkt->sp_Pkt.dp_Res1 != RES_PENDING;
}

uint32_t wos_cda_arrived, wos_cda_arrived_tb;

/* Wait for the one packet in flight; its dp_Res1. */
uint32_t wos_cda_t_wait, wos_cda_t_get, wos_cda_t_inv;
static LONG reply(void)
{
    uint32_t t0 = wos_tb_lo(), t1, t2;
    /* answered already (dp_Res1 set): GetMsg takes it without a WaitPort,
     * a 68k call of 2-5 ms here even with the message in the port */
    if (!arrived())
        WaitPort(s_reply);
    t1 = wos_tb_lo();
    GetMsg(s_reply);
    t2 = wos_tb_lo();
    wos_cache_invalidate_data(s_pkt, sizeof(*s_pkt));
    wos_cda_t_wait += t1 - t0;
    wos_cda_t_get += t2 - t1;
    return s_pkt->sp_Pkt.dp_Res1;
}

/* Starts reading RUN_SECTORS from sector into buffer b; the file is
 * positioned there first if it is not already. */
static int start_read_n(int b, int sector, int n)
{
    uint32_t at = (uint32_t)sector * SECTOR;
    if (at != s_pos) {
        send(ACTION_SEEK, (LONG)at, OFFSET_BEGINNING);
        if (reply() < 0)
            return -1;
        s_pos = at;
    }
    s_count[b] = 0;
    wos_cache_flush_data(s_buf[b], (uint32_t)n * SECTOR);
    send(ACTION_READ, (LONG)s_buf[b], (LONG)n * SECTOR);
    s_send_tb = wos_tb_lo();
    s_pending = b;
    s_pend_first = sector;
    s_pend_len = n * SECTOR;
    s_req[b] = n;
    wos_prof_reads += 0x100;        /* read-ahead fills, bits 8-15 */
    return 0;
}

static int start_read(int b, int sector, int n)
{
    if (s_read_max < 0) {
        s_read_max = wos_env_int("WSX_CDRUN", READ_MAX);
        if (s_read_max < JUMP_SECTORS) s_read_max = JUMP_SECTORS;
        if (s_read_max > RUN_SECTORS) s_read_max = RUN_SECTORS;
    }
    if (n > s_read_max)
        n = s_read_max;
    return start_read_n(b, sector, n);
}

static void finish_read(void)
{
    int b = s_pending;
    LONG n;
    if (b < 0)
        return;
    n = reply();
    s_pending = -1;
    {
        uint32_t t0 = wos_tb_lo();
        wos_cache_invalidate_data(s_buf[b], (uint32_t)s_pend_len);
        wos_cda_t_inv += wos_tb_lo() - t0;
    }
    if (n < 0)
        n = 0;
    s_pos += (uint32_t)n;
    if (n < s_pend_len)             /* the end of the file, or an error */
        s_pos = 0xffffffffu;        /* seek before the next read */
    s_first[b] = s_pend_first;
    s_count[b] = (int)n / SECTOR;
}

static int holds(int b, int sector)
{
    return s_count[b] && sector >= s_first[b] && sector < s_first[b] + s_count[b];
}

int wos_cda_open(const char *path)
{
    wos_cda_close();
    s_fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!s_fh)
        return 0;
    s_fhp = (struct FileHandle *)BADDR(s_fh);
    if (!s_fhp->fh_Type)            /* NIL: and the like have no handler */
        goto fail;
    s_reply = CreateMsgPort();
    s_pkt = wos_alloc_ppc(sizeof(*s_pkt));
    s_buf[0] = wos_alloc_ppc(RUN_BYTES);
    s_buf[1] = wos_alloc_ppc(RUN_BYTES);
    if (!s_reply || !s_pkt || !s_buf[0] || !s_buf[1])
        goto fail;
    s_pos = 0;
    s_count[0] = s_count[1] = 0;
    s_cur = 0;
    s_pending = -1;
    s_ok = 1;
    return 1;
fail:
    wos_cda_close();
    return 0;
}

void wos_cda_close(void)
{
    if (s_pending >= 0)
        finish_read();
    s_ok = 0;
    if (s_buf[0]) wos_free_ppc(s_buf[0]);
    if (s_buf[1]) wos_free_ppc(s_buf[1]);
    if (s_pkt) wos_free_ppc(s_pkt);
    if (s_reply) DeleteMsgPort(s_reply);
    if (s_fh) Close(s_fh);
    s_buf[0] = s_buf[1] = NULL;
    s_pkt = NULL;
    s_reply = NULL;
    s_fh = 0;
    s_fhp = NULL;
}

int wos_cda_ok(void)
{
    return s_ok;
}

/* [prof]: jumps (sectors neither buffer held), waits for the run in flight,
 * and the time-base ticks of each */
uint32_t wos_cda_jumps, wos_cda_waits, wos_cda_jump_tb, wos_cda_wait_tb;

int wos_cda_read(int sector, void *dest)
{
    int other;
    if (!s_ok || sector < 0)
        return -1;
    if (!holds(s_cur, sector)) {
        uint32_t t0 = wos_tb_lo();
        if (s_pending >= 0) {
            uint32_t age = t0 - s_send_tb, lat;
            if (arrived()) {    /* already there: only the 68k calls */
                uint32_t t2 = wos_tb_lo();
                finish_read();
                wos_cda_arrived++;
                wos_cda_arrived_tb += wos_tb_lo() - t2;
                goto got;
            }
            finish_read();
            lat = wos_tb_lo() - s_send_tb;
            wos_cda_waits++;
            wos_cda_wait_tb += wos_tb_lo() - t0;
            wos_cda_age_tb += age;
            wos_cda_lat_tb += lat;
            if (age > wos_cda_age_max) wos_cda_age_max = age;
            if (lat > wos_cda_lat_max) wos_cda_lat_max = lat;
        }
got:
        if (holds(s_cur ^ 1, sector))
            s_cur ^= 1;
        else {
            uint32_t t1 = wos_tb_lo();
            wos_cda_jumps++;
            if (start_read_n(s_cur, sector, JUMP_SECTORS) < 0)
                return -1;
            finish_read();
            wos_cda_jump_tb += wos_tb_lo() - t1;
            if (!holds(s_cur, sector))
                return -1;
        }
    }
    memcpy(dest, s_buf[s_cur] + (sector - s_first[s_cur]) * SECTOR, SECTOR);

    /* Keep the next run coming while this one is used. */
    other = s_cur ^ 1;
    if (s_pending < 0 && s_pos != 0xffffffffu) {
        int next = s_first[s_cur] + s_count[s_cur];
        if (s_count[s_cur] && s_count[s_cur] == s_req[s_cur] && !holds(other, next))
            start_read(other, next, s_req[s_cur] * 2);
    }
    return SECTOR;
}
