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
static uint32_t s_pos;              /* the file position after the last read */
static int s_ok;

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
    wos_cache_flush_data(p, sizeof(*p));
    PutMsg(s_fhp->fh_Type, &p->sp_Msg);
}

/* Wait for the one packet in flight; its dp_Res1. */
static LONG reply(void)
{
    WaitPort(s_reply);
    GetMsg(s_reply);
    wos_cache_invalidate_data(s_pkt, sizeof(*s_pkt));
    return s_pkt->sp_Pkt.dp_Res1;
}

/* Starts reading RUN_SECTORS from sector into buffer b; the file is
 * positioned there first if it is not already. */
static int start_read(int b, int sector)
{
    uint32_t at = (uint32_t)sector * SECTOR;
    if (at != s_pos) {
        send(ACTION_SEEK, (LONG)at, OFFSET_BEGINNING);
        if (reply() < 0)
            return -1;
        s_pos = at;
    }
    s_count[b] = 0;
    wos_cache_flush_data(s_buf[b], RUN_BYTES);
    send(ACTION_READ, (LONG)s_buf[b], RUN_BYTES);
    s_pending = b;
    s_pend_first = sector;
    s_pend_len = RUN_BYTES;
    wos_prof_reads += 0x100;        /* read-ahead fills, bits 8-15 */
    return 0;
}

static void finish_read(void)
{
    int b = s_pending;
    LONG n;
    if (b < 0)
        return;
    n = reply();
    s_pending = -1;
    wos_cache_invalidate_data(s_buf[b], RUN_BYTES);
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

int wos_cda_read(int sector, void *dest)
{
    int other;
    if (!s_ok || sector < 0)
        return -1;
    if (!holds(s_cur, sector)) {
        if (s_pending >= 0)
            finish_read();
        if (holds(s_cur ^ 1, sector))
            s_cur ^= 1;
        else {
            if (start_read(s_cur, sector) < 0)
                return -1;
            finish_read();
            if (!holds(s_cur, sector))
                return -1;
        }
    }
    memcpy(dest, s_buf[s_cur] + (sector - s_first[s_cur]) * SECTOR, SECTOR);

    /* Keep the next run coming while this one is used. */
    other = s_cur ^ 1;
    if (s_pending < 0 && s_pos != 0xffffffffu) {
        int next = s_first[s_cur] + s_count[s_cur];
        if (s_count[s_cur] == RUN_SECTORS && !holds(other, next))
            start_read(other, next);
    }
    return SECTOR;
}
