/*
 * wos_xadecode.c -- xaDecode.S in C, for the WarpOS build.
 *
 * The Wii decodes the PSX's 4-bit ADPCM (CD-XA audio in decode_xa.c, SPU
 * voices in dfsound) with Gekko paired-single code; the 603e and G4 have no
 * paired singles. Same entry points, same arithmetic: 28 samples a block,
 * each  y = x + IK0 * y[-1] + IK1 * y[-2]  in single precision, the history
 * kept unclamped as a float, each output saturated to 16 bits.
 *
 * `inc` is in BYTES between two output samples, as the asm takes it.
 */
#include "../psxcommon.h"

static inline s16 sat16(float v)
{
    int i = (int)v;                 /* psq_st truncates */
    if (i > 32767)  return 32767;
    if (i < -32768) return -32768;
    return (s16)i;
}

/* One decoded nibble: in the top bits of a halfword, shifted down by `range`. */
static inline float nibble(u32 bits, u32 range)
{
    return (float)((s32)(s16)(bits & 0xf000) >> range);
}

/* CD-XA: 14 native halfwords, nibbles from the low end up. The first output
 * sample goes to destp. */
void psDecodeBlock16(void *tmp, s16 *destp, s32 inc)
{
    typPcmDecode *d = (typPcmDecode *)tmp;
    const u16 *src = (const u16 *)(uintptr_t)d->vblockp;
    f32 *state = (f32 *)(uintptr_t)d->decpAddr;
    const float k0 = d->IK0, k1 = d->IK1;
    const u32 range = d->range;
    float y0 = state[0], y1 = state[1];
    u8 *out = (u8 *)destp;
    int i, n;

    for (i = 0; i < 7; i++) {
        u32 y = src[i];
        float x[4];
        x[0] = nibble(y << 12, range);
        x[1] = nibble(y << 8,  range);
        x[2] = nibble(y << 4,  range);
        x[3] = nibble(y,       range);
        for (n = 0; n < 4; n++) {
            float v = x[n] + k0 * y0 + k1 * y1;
            y1 = y0;
            y0 = v;
            *(s16 *)out = sat16(v);
            out += inc;
        }
    }
    state[0] = y0;
    state[1] = y1;
}

/* SPU RAM: 14 bytes as they lie in memory, the low nibble of each first. The
 * caller passes dest - 1 and a 2-byte step, so the first sample lands on dest. */
void psDecodePcmBlock(void *tmp, s16 *destp, s32 inc)
{
    typPcmDecode *d = (typPcmDecode *)tmp;
    const u8 *src = (const u8 *)(uintptr_t)d->vblockp;
    f32 *state = (f32 *)(uintptr_t)d->decpAddr;
    const float k0 = d->IK0, k1 = d->IK1;
    const u32 range = d->range;
    float y0 = state[0], y1 = state[1];
    u8 *out = (u8 *)destp + inc;
    int i, n;

    for (i = 0; i < 14; i++) {
        u32 b = src[i];
        float x[2];
        x[0] = nibble(b << 12, range);
        x[1] = nibble(b << 8,  range);
        for (n = 0; n < 2; n++) {
            float v = x[n] + k0 * y0 + k1 * y1;
            y1 = y0;
            y0 = v;
            *(s16 *)out = sat16(v);
            out += inc;
        }
    }
    state[0] = y0;
    state[1] = y1;
}
