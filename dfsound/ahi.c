/*
 * ahi.c -- dfsound's output driver for WarpOS: AHI, as a ring it plays for ever.
 *
 * The SPU's mix (44.1 kHz, 16-bit stereo, native byte order) goes into one
 * dynamic sample, looped on one channel, the way the WarpOS Quake ports and
 * OpenSWOS do it. Where AHI is playing comes from an AHIET_CHANNELINFO effect
 * hook that AHI calls on every mix with the play offset; we write ahead of it.
 * A feed makes NO call to the 68k: the PPC writes the ring, pushes those cache
 * lines out (dcbf: the 68k does not see the PPC's data cache) and reads the
 * play position the hook left in memory, past its own cached copy.
 *
 * AHI mixes on the 68k: SetEnv WSX_AHI_MIXRATE 22050 halves its work.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "out.h"
#include "../WarpOS/wos_sys.h"

#pragma pack(push, 2)
#include <exec/types.h>
#include <exec/memory.h>
#include <utility/hooks.h>
#include <devices/ahi.h>
#include <proto/exec.h>
#include <proto/ahi.h>
#pragma pack(pop)

char audioEnabled;

/* The AHIET_CHANNELINFO hook, as 68k code: AHI calls it on its own side.
 *     hook->h_Data = info->ahieci_Offset[0]; return 0;
 * (movem.l a2,-(sp); movea.l a0,a2; move.l 12(a1),d0; move.l d0,16(a2);
 *  moveq #0,d0; movea.l (sp)+,a2; rts) */
static unsigned short s_hook_code[] = {
    0x48e7, 0x0020, 0x2448, 0x2029, 0x000c, 0x2540, 0x0010, 0x7000, 0x245f, 0x4e75
};

#pragma pack(push, 2)
struct ChannelInfo {
    struct AHIEffChannelInfo cinfo;
    ULONG                    x[1];          /* ahieci_Offset[] for the one channel */
};
#pragma pack(pop)

#define SPU_RATE      44100
#define RING_FRAMES   32768                 /* 0.74 s; a power of two */
#define FRAME_BYTES   4                     /* 16-bit stereo */
#define LEAD_FRAMES   1024                  /* where writing restarts after an underrun */
#define BUSY_FRAMES   (SPU_RATE / 10)       /* "enough queued": 100 ms */

struct Library *AHIBase;
static struct MsgPort      *s_port;
static struct AHIRequest   *s_req;
static int                  s_dev_open;
static struct AHIAudioCtrl *s_ctrl;
static int                  s_sound_loaded;
static int                  s_effect_on;
static struct ChannelInfo   s_info;
static struct Hook          s_hook;
static int16_t             *s_ring;         /* RING_FRAMES * 2 samples */
static uint32_t             s_write;        /* frames written; the ring index is & (RING_FRAMES-1) */
static int                  s_open;

/* Where AHI is playing, in frames into the ring: the hook's last word, read past
 * any stale copy of it in the PPC's cache. */
static uint32_t play_frame(void)
{
    wos_cache_invalidate_data(&s_hook.h_Data, sizeof s_hook.h_Data);
    return (uint32_t)(uintptr_t)s_hook.h_Data & (RING_FRAMES - 1);
}

static void restart_writer(uint32_t p)
{
    s_write = (s_write & ~(uint32_t)(RING_FRAMES - 1)) + ((p + LEAD_FRAMES) & (RING_FRAMES - 1));
}

/* Frames written and not yet played. A writer that fell behind (the emulator
 * stalled longer than half the ring) is put back just ahead of the player. */
static uint32_t ahead(void)
{
    uint32_t p = play_frame(), w = s_write & (RING_FRAMES - 1);
    uint32_t d = (w - p) & (RING_FRAMES - 1);
    if (d > RING_FRAMES / 2) {
        restart_writer(p);
        d = LEAD_FRAMES;
    }
    return d;
}

static void ahi_finish(void);

static int ahi_init(void)
{
    struct AHISampleInfo sample;
    int mixrate = wos_env_int("WSX_AHI_MIXRATE", SPU_RATE);

    if (mixrate < 8000 || mixrate > 48000)
        mixrate = SPU_RATE;
    if (!audioEnabled) {
        wos_log("[audio] disabled");
        return 0;           /* a silent driver: feed and busy do nothing */
    }

    if (!(s_port = CreateMsgPort()))
        goto fail;
    if (!(s_req = (struct AHIRequest *)CreateIORequest(s_port, sizeof(struct AHIRequest))))
        goto fail;
    s_req->ahir_Version = 4;
    if (OpenDevice((CONST_STRPTR)AHINAME, AHI_NO_UNIT, (struct IORequest *)s_req, 0) != 0) {
        wos_log("[audio] no ahi.device v4: playing silent");
        goto fail;
    }
    s_dev_open = 1;
    AHIBase = (struct Library *)s_req->ahir_Std.io_Device;

    s_ctrl = AHI_AllocAudio(AHIA_AudioID, AHI_DEFAULT_ID,
                            AHIA_MixFreq, (ULONG)mixrate,
                            AHIA_Channels, 1,
                            AHIA_Sounds, 1,
                            TAG_END);
    if (!s_ctrl) {
        wos_log("[audio] AHI_AllocAudio failed (the default mode in AHI prefs?)");
        goto fail;
    }
    s_ring = (int16_t *)AllocVec(RING_FRAMES * FRAME_BYTES, MEMF_PUBLIC | MEMF_CLEAR);
    if (!s_ring)
        goto fail;
    wos_cache_flush_data(s_ring, RING_FRAMES * FRAME_BYTES);

    sample.ahisi_Type    = AHIST_S16S;
    sample.ahisi_Address = s_ring;
    sample.ahisi_Length  = RING_FRAMES;
    if (AHI_LoadSound(0, AHIST_DYNAMICSAMPLE, &sample, s_ctrl) != 0)
        goto fail;
    s_sound_loaded = 1;
    if (AHI_ControlAudio(s_ctrl, AHIC_Play, TRUE, TAG_END) != 0)
        goto fail;

    s_hook.h_Entry = (HOOKFUNC)s_hook_code;
    s_hook.h_Data = 0;
    s_info.cinfo.ahie_Effect = AHIET_CHANNELINFO;
    s_info.cinfo.ahieci_Func = &s_hook;
    s_info.cinfo.ahieci_Channels = 1;
    wos_cache_flush_data(s_hook_code, sizeof s_hook_code);
    wos_cache_flush_data(&s_hook, sizeof s_hook);
    wos_cache_flush_data(&s_info, sizeof s_info);

    AHI_Play(s_ctrl,
             AHIP_BeginChannel, 0,
             AHIP_Freq,         (ULONG)SPU_RATE,
             AHIP_Vol,          0x10000,
             AHIP_Pan,          0x8000,
             AHIP_Sound,        0,
             AHIP_EndChannel,   0,
             TAG_END);
    AHI_SetEffect(&s_info, s_ctrl);
    s_effect_on = 1;

    s_write = LEAD_FRAMES;
    s_open = 1;
    wos_log("[audio] AHI: 16-bit stereo %d Hz, mixed at %d Hz, a ring of %d frames",
            SPU_RATE, mixrate, RING_FRAMES);
    return 0;

fail:
    ahi_finish();
    return 0;               /* silent, but the emulator still runs */
}

static void ahi_finish(void)
{
    if (s_ctrl) {
        if (s_effect_on) {
            s_info.cinfo.ahie_Effect = AHIET_CHANNELINFO | AHIET_CANCEL;
            AHI_SetEffect(&s_info, s_ctrl);
            s_effect_on = 0;
        }
        AHI_ControlAudio(s_ctrl, AHIC_Play, FALSE, TAG_END);
        if (s_sound_loaded) {
            AHI_UnloadSound(0, s_ctrl);
            s_sound_loaded = 0;
        }
        AHI_FreeAudio(s_ctrl);
        s_ctrl = NULL;
    }
    if (s_ring) {
        FreeVec(s_ring);
        s_ring = NULL;
    }
    if (s_dev_open) {
        CloseDevice((struct IORequest *)s_req);
        s_dev_open = 0;
    }
    if (s_req) {
        DeleteIORequest((struct IORequest *)s_req);
        s_req = NULL;
    }
    if (s_port) {
        DeleteMsgPort(s_port);
        s_port = NULL;
    }
    AHIBase = NULL;
    s_open = 0;
}

static int ahi_busy(void)
{
    if (!s_open)
        return 1;           /* silent: never ask the SPU for more */
    return ahead() > BUSY_FRAMES;
}

static int ahi_feed(void *data, int bytes)
{
    const int16_t *pcm = (const int16_t *)data;
    uint32_t frames, w, first;

    if (!s_open || bytes <= 0)
        return 0;
    frames = (uint32_t)bytes / FRAME_BYTES;
    if (frames > RING_FRAMES / 2)
        frames = RING_FRAMES / 2;
    ahead();                /* re-sync the writer after a stall */
    w = s_write & (RING_FRAMES - 1);
    first = RING_FRAMES - w;
    if (first > frames)
        first = frames;
    memcpy(s_ring + w * 2, pcm, first * FRAME_BYTES);
    wos_cache_flush_data(s_ring + w * 2, first * FRAME_BYTES);
    if (frames > first) {
        memcpy(s_ring, pcm + first * 2, (frames - first) * FRAME_BYTES);
        wos_cache_flush_data(s_ring, (frames - first) * FRAME_BYTES);
    }
    s_write += frames;
    return 0;
}

void pauseAudio(void)  {}
void resumeAudio(void) {}

void out_register_ahi(struct out_driver *drv)
{
    drv->name   = "ahi";
    drv->init   = ahi_init;
    drv->finish = ahi_finish;
    drv->busy   = ahi_busy;
    drv->feed   = ahi_feed;
}
