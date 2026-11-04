/* avbench: the AltiVec kernels (built with -mcpu=7400 -maltivec).
 * Every buffer is 16-byte aligned (32 for the dcbz variants) and every
 * length a multiple of the kernel's step. No vector values cross a call:
 * the startup code's stack alignment is not trusted. */
#include <altivec.h>
#include <stddef.h>

typedef vector unsigned char  vu8;
typedef vector unsigned short vu16;
typedef vector signed short   vs16;
typedef vector unsigned int   vu32;
typedef vector signed int     vs32;
typedef vector float          vf;

/* 64 bytes a turn */
void v_copy(void *dst, const void *src, size_t n)
{
    vu8 *d = dst;
    const vu8 *s = src;
    for (n /= 64; n; n--, s += 4, d += 4) {
        vu8 a = vec_ld(0, s), b = vec_ld(16, s), c = vec_ld(32, s), e = vec_ld(48, s);
        vec_st(a, 0, d); vec_st(b, 16, d); vec_st(c, 32, d); vec_st(e, 48, d);
    }
}

/* the same, with a dcbz on each destination line first (no read of a line
 * that is about to be overwritten) and a dcbt 128 bytes ahead on the source */
void v_copy_dcbz(void *dst, const void *src, size_t n)
{
    vu8 *d = dst;
    const vu8 *s = src;
    for (n /= 64; n; n--, s += 4, d += 4) {
        vu8 a, b, c, e;
        __asm__ volatile("dcbz 0,%0\n\tdcbz %1,%0\n\tdcbt %2,%3"
                         : : "r"(d), "b"(32), "b"(128), "r"(s) : "memory");
        a = vec_ld(0, s); b = vec_ld(16, s); c = vec_ld(32, s); e = vec_ld(48, s);
        vec_st(a, 0, d); vec_st(b, 16, d); vec_st(c, 32, d); vec_st(e, 48, d);
    }
}

void v_clear(void *dst, size_t n)
{
    vu8 *d = dst;
    vu8 z = vec_splat_u8(0);
    for (n /= 64; n; n--, d += 4) {
        vec_st(z, 0, d); vec_st(z, 16, d); vec_st(z, 32, d); vec_st(z, 48, d);
    }
}

/* 4-bit palette texels (low nibble first, as the PSX stores them) to
 * 32-bit palette entries: 32 texels a turn. n: input bytes (x16). */
/* kc: p0..p3, 16-byte aligned at run time (the hunk loader aligns sections
 * to 8 bytes only, and lvx drops the low 4 address bits) */
void v_clut4(unsigned *out, const unsigned char *in, const unsigned *clut, size_t n, const void *kc)
{
    const vu8 *cv = (const vu8 *)clut;          /* 16 entries = 4 vectors */
    vu8 c0 = vec_ld(0, cv), c1 = vec_ld(16, cv), c2 = vec_ld(32, cv), c3 = vec_ld(48, cv);
    /* byte planes: Tk[i] = byte k of entry i */
    vu8 t0, t1, t2, t3;
    const vu8 p0 = vec_ld(0, (const vu8 *)kc), p1 = vec_ld(16, (const vu8 *)kc),
              p2 = vec_ld(32, (const vu8 *)kc), p3 = vec_ld(48, (const vu8 *)kc);
    vu8 nib = vec_splat_u8(15), four = vec_splat_u8(4);
    vu8 *o = (vu8 *)out;
    const vu8 *s = (const vu8 *)in;
    /* entries 0-7 from c0,c1 and 8-15 from c2,c3: a[0..7] b[0..7] */
    {
        vu8 a = vec_perm(c0, c1, p0), b = vec_perm(c2, c3, p0);
        t0 = vec_sld(vec_sld(a, a, 8), b, 8);           /* a[0..7] b[0..7] */
        a = vec_perm(c0, c1, p1); b = vec_perm(c2, c3, p1);
        t1 = vec_sld(vec_sld(a, a, 8), b, 8);
        a = vec_perm(c0, c1, p2); b = vec_perm(c2, c3, p2);
        t2 = vec_sld(vec_sld(a, a, 8), b, 8);
        a = vec_perm(c0, c1, p3); b = vec_perm(c2, c3, p3);
        t3 = vec_sld(vec_sld(a, a, 8), b, 8);
    }
    for (n /= 16; n; n--, s++, o += 8) {
        vu8 v = vec_ld(0, s);
        vu8 lo = vec_and(v, nib), hi = vec_sr(v, four);
        vu8 ia = vec_mergeh(lo, hi), ib = vec_mergel(lo, hi);
        vu8 b0, b1, b2, b3, m01h, m01l, m23h, m23l;

        b0 = vec_perm(t0, t0, ia); b1 = vec_perm(t1, t1, ia);
        b2 = vec_perm(t2, t2, ia); b3 = vec_perm(t3, t3, ia);
        m01h = vec_mergeh(b0, b1); m01l = vec_mergel(b0, b1);
        m23h = vec_mergeh(b2, b3); m23l = vec_mergel(b2, b3);
        vec_st((vu8)vec_mergeh((vu16)m01h, (vu16)m23h), 0, o);
        vec_st((vu8)vec_mergel((vu16)m01h, (vu16)m23h), 16, o);
        vec_st((vu8)vec_mergeh((vu16)m01l, (vu16)m23l), 32, o);
        vec_st((vu8)vec_mergel((vu16)m01l, (vu16)m23l), 48, o);

        b0 = vec_perm(t0, t0, ib); b1 = vec_perm(t1, t1, ib);
        b2 = vec_perm(t2, t2, ib); b3 = vec_perm(t3, t3, ib);
        m01h = vec_mergeh(b0, b1); m01l = vec_mergel(b0, b1);
        m23h = vec_mergeh(b2, b3); m23l = vec_mergel(b2, b3);
        vec_st((vu8)vec_mergeh((vu16)m01h, (vu16)m23h), 64, o);
        vec_st((vu8)vec_mergel((vu16)m01h, (vu16)m23h), 80, o);
        vec_st((vu8)vec_mergeh((vu16)m01l, (vu16)m23l), 96, o);
        vec_st((vu8)vec_mergel((vu16)m01l, (vu16)m23l), 112, o);
    }
}

/* PSX 15-bit colours (big-endian halfwords here) to B,G,R,A bytes;
 * A = 0 for 0x0000 (transparent), else 0xff. 16 pixels a turn. */
/* kc: 8 x 31 and 8 x 0x00ff halfwords, aligned at run time */
void v_c15(unsigned *out, const unsigned short *in, size_t npix, const void *kc)
{
    const vu16 *s = (const vu16 *)in;
    vu8 *o = (vu8 *)out;
    vu16 m5 = vec_ld(0, (const vu16 *)kc), ff = vec_ld(16, (const vu16 *)kc),
         three = vec_splat_u16(3), five = vec_splat_u16(5), ten = vec_splat_u16(10),
         z = vec_splat_u16(0);
    for (npix /= 16; npix; npix--, s += 2, o += 4) {
        vu16 x0 = vec_ld(0, s), x1 = vec_ld(16, s);
        vu8 R = vec_pack(vec_sl(vec_and(x0, m5), three), vec_sl(vec_and(x1, m5), three));
        vu8 G = vec_pack(vec_sl(vec_and(vec_sr(x0, five), m5), three),
                         vec_sl(vec_and(vec_sr(x1, five), m5), three));
        vu8 B = vec_pack(vec_sl(vec_and(vec_sr(x0, ten), m5), three),
                         vec_sl(vec_and(vec_sr(x1, ten), m5), three));
        vu8 A = vec_pack(vec_andc(ff, (vu16)vec_cmpeq(x0, z)), vec_andc(ff, (vu16)vec_cmpeq(x1, z)));
        vu8 bgh = vec_mergeh(B, G), bgl = vec_mergel(B, G);
        vu8 rah = vec_mergeh(R, A), ral = vec_mergel(R, A);
        vec_st((vu8)vec_mergeh((vu16)bgh, (vu16)rah), 0, o);
        vec_st((vu8)vec_mergel((vu16)bgh, (vu16)rah), 16, o);
        vec_st((vu8)vec_mergeh((vu16)bgl, (vu16)ral), 32, o);
        vec_st((vu8)vec_mergel((vu16)bgl, (vu16)ral), 48, o);
    }
}

/* acc[i] += (s[i] * vol) >> 15, 8 samples a turn */
void v_mix(int *acc, const short *smp, int vol, size_t n)
{
    vs16 v;
    vu32 fifteen = vec_splat_u32(15);
    const vs16 *s = (const vs16 *)smp;
    vs32 *a = (vs32 *)acc;
    union { short h[8]; vs16 v; } u __attribute__((aligned(16)));
    int i;
    for (i = 0; i < 8; i++) u.h[i] = (short)vol;
    v = u.v;
    for (n /= 8; n; n--, s++, a += 2) {
        vs16 x = vec_ld(0, s);
        vs32 e = vec_sra(vec_mule(x, v), fifteen), od = vec_sra(vec_mulo(x, v), fifteen);
        vec_st(vec_add(vec_ld(0, a), vec_mergeh(e, od)), 0, a);
        vec_st(vec_add(vec_ld(16, a), vec_mergel(e, od)), 16, a);
    }
}

/* out = M * in for n vertices of 4 floats; m: 4 columns of 4 */
void v_xform(float *out, const float *in, const float *m, size_t n)
{
    vf c0 = vec_ld(0, m), c1 = vec_ld(16, m), c2 = vec_ld(32, m), c3 = vec_ld(48, m);
    vf zero = (vf)vec_splat_u32(0);
    const vf *s = (const vf *)in;
    vf *o = (vf *)out;
    for (; n; n--, s++, o++) {
        vf x = vec_ld(0, s);
        vf r = vec_madd(c0, vec_splat(x, 0), zero);
        r = vec_madd(c1, vec_splat(x, 1), r);
        r = vec_madd(c2, vec_splat(x, 2), r);
        r = vec_madd(c3, vec_splat(x, 3), r);
        vec_st(r, 0, o);
    }
}

/* the stack pointer's alignment as this file's code sees it */
unsigned v_stack_mod16(void)
{
    return (unsigned)(size_t)__builtin_frame_address(0) & 15;
}

unsigned v_vscr(void)
{
    static unsigned w[4] __attribute__((aligned(16)));
    vec_st((vu32)vec_mfvscr(), 0, w);
    return w[3];
}
