/* avbench -- what AltiVec does on this WarpOS machine.
 *
 *   avbench info            CPU, clocks, caches, MSR[VEC]
 *   avbench bench           scalar against AltiVec kernels, results checked
 *   avbench stress s seed   v0-v30 loaded and checked for s seconds: two
 *                           copies at once show whether a task switch keeps
 *                           each task's vector registers
 *
 * This file is 603e code; the vector kernels are in avvec.c (7400). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <powerpc/powerpc.h>
#include <powerpc/powerpc_protos.h>

extern struct Library *PowerPCBase;

void v_copy(void *dst, const void *src, size_t n);
void v_copy_dcbz(void *dst, const void *src, size_t n);
void v_clear(void *dst, size_t n);
void v_clut4(unsigned *out, const unsigned char *in, const unsigned *clut, size_t n, const void *kc);
void v_c15(unsigned *out, const unsigned short *in, size_t npix, const void *kc);
void v_mix(int *acc, const short *smp, int vol, size_t n);
void v_xform(float *out, const float *in, const float *m, size_t n);
unsigned v_stack_mod16(void);
unsigned v_vscr(void);
int av_stress(const unsigned *pat, unsigned iters);
void av_aligned(void *fn, ...);
static int s_align;     /* bench a: vector kernels through av_aligned */
#define VCALL(f, ...) do { if (s_align) av_aligned((void *)(f), __VA_ARGS__); else f(__VA_ARGS__); } while (0)

/* ---- time ---------------------------------------------------------------- */

static unsigned long long tb(void)
{
    unsigned hi, lo, hi2;
    do {
        __asm__ volatile("mftbu %0" : "=r"(hi));
        __asm__ volatile("mftb %0" : "=r"(lo));
        __asm__ volatile("mftbu %0" : "=r"(hi2));
    } while (hi != hi2);
    return ((unsigned long long)hi << 32) | lo;
}

static double s_tb_hz = 24740000.0;

static void tb_calibrate(void)
{
    struct DateStamp a, b;
    unsigned long long t0, t1;
    long ticks;
    DateStamp(&a);
    do DateStamp(&b); while (b.ds_Tick == a.ds_Tick);   /* a tick edge */
    t0 = tb();
    do DateStamp(&a); while ((a.ds_Minute * 3000 + a.ds_Tick) - (b.ds_Minute * 3000 + b.ds_Tick) < 50);
    t1 = tb();
    ticks = (a.ds_Minute * 3000 + a.ds_Tick) - (b.ds_Minute * 3000 + b.ds_Tick);
    s_tb_hz = (double)(t1 - t0) * 50.0 / (double)ticks;
}

static double us_of(unsigned long long t) { return (double)t * 1e6 / s_tb_hz; }

/* ---- memory ------------------------------------------------------------- */

static void *amalloc(size_t n)
{
    unsigned char *p = malloc(n + 64);
    if (!p) { printf("out of memory (%lu)\n", (unsigned long)n); exit(20); }
    p = (unsigned char *)(((size_t)p + 63) & ~(size_t)63);
    return p;
}

static unsigned s_seed = 12345;
static unsigned rnd(void) { s_seed = s_seed * 1103515245u + 12345u; return s_seed >> 8 ^ s_seed << 16; }
static void fill(void *p, size_t n) { unsigned char *c = p; while (n--) *c++ = (unsigned char)rnd(); }

/* ---- scalar kernels ------------------------------------------------------ */

static void s_copy_words(void *dst, const void *src, size_t n)
{
    unsigned *d = dst; const unsigned *s = src;
    for (n /= 32; n; n--, s += 8, d += 8) {
        unsigned a = s[0], b = s[1], c = s[2], e = s[3], f = s[4], g = s[5], h = s[6], i = s[7];
        d[0] = a; d[1] = b; d[2] = c; d[3] = e; d[4] = f; d[5] = g; d[6] = h; d[7] = i;
    }
}

static void s_copy_fpr(void *dst, const void *src, size_t n)   /* 64-bit loads/stores */
{
    double *d = dst; const double *s = src;
    for (n /= 32; n; n--, s += 4, d += 4) {
        double a = s[0], b = s[1], c = s[2], e = s[3];
        d[0] = a; d[1] = b; d[2] = c; d[3] = e;
    }
}

static void s_clut4(unsigned *out, const unsigned char *in, const unsigned *clut, size_t n)
{
    while (n--) {
        unsigned b = *in++;
        out[0] = clut[b & 15];
        out[1] = clut[b >> 4];
        out += 2;
    }
}

static void s_c15(unsigned *out, const unsigned short *in, size_t npix)
{
    while (npix--) {
        unsigned x = *in++;
        unsigned r = (x & 31) << 3, g = ((x >> 5) & 31) << 3, b = ((x >> 10) & 31) << 3;
        unsigned a = x ? 0xff : 0;
        *out++ = (b << 24) | (g << 16) | (r << 8) | a;
    }
}

static void s_mix(int *acc, const short *smp, int vol, size_t n)
{
    while (n--)
        *acc++ += (*smp++ * vol) >> 15;
}

static void s_xform(float *out, const float *in, const float *m, size_t n)
{
    for (; n; n--, in += 4, out += 4) {
        float x = in[0], y = in[1], z = in[2], w = in[3];
        out[0] = m[0] * x + m[4] * y + m[8] * z + m[12] * w;
        out[1] = m[1] * x + m[5] * y + m[9] * z + m[13] * w;
        out[2] = m[2] * x + m[6] * y + m[10] * z + m[14] * w;
        out[3] = m[3] * x + m[7] * y + m[11] * z + m[15] * w;
    }
}

/* ---- info ---------------------------------------------------------------- */

static unsigned read_msr(void)
{
    unsigned old = Super(), msr;
    __asm__ volatile("mfmsr %0" : "=r"(msr));
    User(old);
    return msr;
}

static void info(void)
{
    struct TagItem t[] = {
        { GETINFO_CPU, 0 }, { GETINFO_PVR, 0 }, { GETINFO_CPUCLOCK, 0 }, { GETINFO_BUSCLOCK, 0 },
        { GETINFO_L2CACHE, 0 }, { GETINFO_L2STATE, 0 }, { GETINFO_L2SIZE, 0 },
        { GETINFO_ICACHE, 0 }, { GETINFO_DCACHE, 0 }, { TAG_END, 0 }
    };
    unsigned vrsave;
    GetInfo(t);
    printf("powerpc.library %u.%u\n", PowerPCBase->lib_Version, PowerPCBase->lib_Revision);
    printf("cpu %08lx pvr %08lx cpu clock %lu bus clock %lu\n", t[0].ti_Data, t[1].ti_Data,
           t[2].ti_Data, t[3].ti_Data);
    printf("L2 %08lx state %08lx size %lu; icache %08lx dcache %08lx\n", t[4].ti_Data, t[5].ti_Data,
           t[6].ti_Data, t[7].ti_Data, t[8].ti_Data);
    printf("time base %.0f Hz (measured)\n", s_tb_hz);
    printf("MSR %08x before any vector instruction (VEC bit %u)\n", read_msr(), (read_msr() >> 25) & 1);
    fflush(stdout);
    printf("stack mod 16 in vector code: %u\n", v_stack_mod16());
    __asm__ volatile("mfspr %0,256" : "=r"(vrsave));
    printf("VRSAVE %08x\n", vrsave);
    {
        static unsigned char sa[64] __attribute__((aligned(16)));
        unsigned char *a = amalloc(64), *b = amalloc(64);
        printf("a static 'aligned(16)' array sits at an address mod 16 = %u\n", (unsigned)((size_t)sa & 15));
        fill(a, 64);
        printf("first vector copy...\n"); fflush(stdout);
        v_copy(b, a, 64);
        printf("done, %s; MSR %08x (VEC bit %u)\n", memcmp(a, b, 64) ? "WRONG" : "right",
               read_msr(), (read_msr() >> 25) & 1);
    }
    {
        unsigned vscr = v_vscr();
        printf("VSCR %08x (NJ bit %u)\n", vscr, (vscr >> 16) & 1);
    }
}

/* ---- bench --------------------------------------------------------------- */

#define REPS 7

typedef void (*fn_t)(void *ctx);

static double best_us(fn_t f, void *ctx)
{
    double best = 1e30;
    int i;
    for (i = 0; i < REPS; i++) {
        unsigned long long t0 = tb();
        double us;
        f(ctx);
        us = us_of(tb() - t0);
        if (us < best) best = us;
    }
    return best;
}

struct cctx { void *d; const void *s; size_t n; void (*k)(void *, const void *, size_t); };
static void run_copy(void *c) { struct cctx *x = c; x->k(x->d, x->s, x->n); }
static void k_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
static void run_memset(void *c) { struct cctx *x = c; memset(x->d, 0, x->n); }
static void run_vclear(void *c) { struct cctx *x = c; v_clear(x->d, x->n); }
static void run_dcbz(void *c)
{
    struct cctx *x = c;
    unsigned char *p = x->d;
    size_t i;
    for (i = 0; i < x->n; i += 32)
        __asm__ volatile("dcbz 0,%0" : : "r"(p + i) : "memory");
}

static void diff(const char *what, const void *a, const void *b, size_t n)
{
    const unsigned *x = a, *y = b;
    size_t i, bad = 0, first = 0;
    for (i = 0; i < n / 4; i++)
        if (x[i] != y[i]) { if (!bad++) first = i; }
    if (bad)
        printf("   %s: %lu of %lu words differ, first at %lu: %08x / %08x\n", what, (unsigned long)bad,
               (unsigned long)(n / 4), (unsigned long)first, x[first], y[first]);
}

static void line(const char *what, double us_s, double us_v, double mb, int ok)
{
    printf("%-34s scalar %9.1f us %7.1f MB/s | AltiVec %9.1f us %7.1f MB/s | x%.2f %s\n",
           what, us_s, mb / us_s, us_v, mb / us_v, us_s / us_v, ok ? "same" : "DIFFERENT");
    fflush(stdout);
}

static void bench_copy(size_t n)
{
    unsigned char *a = amalloc(n), *b = amalloc(n), *c = amalloc(n);
    struct cctx x = { 0 };
    double t_mc, t_w, t_f, t_v, t_vz;
    char what[64];
    fill(a, n);
    x.s = a; x.n = n;
    x.d = b; x.k = k_memcpy;      t_mc = best_us(run_copy, &x);
    x.k = s_copy_words;           t_w = best_us(run_copy, &x);
    x.k = s_copy_fpr;             t_f = best_us(run_copy, &x);
    x.d = c; x.k = v_copy;        t_v = best_us(run_copy, &x);
    x.k = v_copy_dcbz;            t_vz = best_us(run_copy, &x);
    snprintf(what, sizeof what, "copy %lu KB (memcpy)", (unsigned long)(n >> 10));
    line(what, t_mc, t_v, (double)n, !memcmp(b, c, n));
    snprintf(what, sizeof what, "copy %lu KB (words / fpr)", (unsigned long)(n >> 10));
    line(what, t_w < t_f ? t_w : t_f, t_v, (double)n, !memcmp(b, c, n));
    printf("%-34s words %.1f us, fpr %.1f us, AltiVec+dcbz %.1f us (%.1f MB/s)\n", "", t_w, t_f,
           t_vz, (double)n / t_vz);
    x.d = b; t_mc = best_us(run_memset, &x);
    x.d = c; t_v = best_us(run_vclear, &x);
    t_vz = best_us(run_dcbz, &x);
    snprintf(what, sizeof what, "clear %lu KB (memset)", (unsigned long)(n >> 10));
    line(what, t_mc, t_v, (double)n, !memcmp(b, c, n));
    printf("%-34s dcbz alone %.1f us (%.1f MB/s)\n", "", t_vz, (double)n / t_vz);
}

struct tctx { unsigned *out; const void *in; const void *aux; size_t n; int vec; const void *kc; };
static void run_clut(void *c)
{
    struct tctx *x = c;
    if (x->vec) VCALL(v_clut4, x->out, x->in, x->aux, x->n, x->kc); else s_clut4(x->out, x->in, x->aux, x->n);
}
static void run_c15(void *c)
{
    struct tctx *x = c;
    if (x->vec) VCALL(v_c15, x->out, x->in, x->n, x->kc); else s_c15(x->out, x->in, x->n);
}

static void bench_textures(void)
{
    /* a 256 x 256 page of 4-bit texels: 32 KB in, 256 KB out */
    size_t nb = 256 * 256 / 2, np = 256 * 256;
    unsigned char *in = amalloc(np * 2);
    unsigned *o1 = amalloc(np * 4), *o2 = amalloc(np * 4);
    unsigned *clut = amalloc(64);
    struct tctx x;
    double ts, tv;
    unsigned char *kclut = amalloc(64);
    unsigned short *k15 = amalloc(32);
    int i;
    for (i = 0; i < 16; i++) {          /* p0..p3: byte k of entries 0..7 */
        kclut[i] = (unsigned char)((i & 7) * 4);
        kclut[16 + i] = (unsigned char)((i & 7) * 4 + 1);
        kclut[32 + i] = (unsigned char)((i & 7) * 4 + 2);
        kclut[48 + i] = (unsigned char)((i & 7) * 4 + 3);
    }
    for (i = 0; i < 8; i++) { k15[i] = 31; k15[8 + i] = 0x00ff; }
    fill(in, np * 2);
    fill(clut, 64);
    x.kc = kclut;
    x.in = in; x.aux = clut; x.n = nb;
    x.out = o1; x.vec = 0; ts = best_us(run_clut, &x);
    x.out = o2; x.vec = 1; tv = best_us(run_clut, &x);
    line("4-bit CLUT page 256x256 -> 32-bit", ts, tv, (double)np * 4, !memcmp(o1, o2, np * 4));
    diff("clut", o1, o2, np * 4);
    /* 64 x 64 (a typical sub-texture): the set-up counts */
    x.n = 64 * 64 / 2;
    x.out = o1; x.vec = 0; ts = best_us(run_clut, &x);
    x.out = o2; x.vec = 1; tv = best_us(run_clut, &x);
    line("4-bit CLUT 64x64 -> 32-bit", ts, tv, 64.0 * 64 * 4, !memcmp(o1, o2, 64 * 64 * 4));
    x.n = np; x.kc = k15;
    x.out = o1; x.vec = 0; ts = best_us(run_c15, &x);
    x.out = o2; x.vec = 1; tv = best_us(run_c15, &x);
    line("15-bit 256x256 -> BGRA", ts, tv, (double)np * 4, !memcmp(o1, o2, np * 4));
    diff("c15", o1, o2, np * 4);
}

struct mctx { int *acc; const short *smp; size_t n; int vec; };
static void run_mix(void *c)
{
    struct mctx *x = c;
    int v;
    for (v = 0; v < 24; v++) {        /* 24 voices */
        if (x->vec) VCALL(v_mix, x->acc, x->smp + v * 64, 0x3000 + v * 97, x->n);
        else s_mix(x->acc, x->smp + v * 64, 0x3000 + v * 97, x->n);
    }
}

static void bench_mix(void)
{
    size_t n = 4096;                    /* samples per voice */
    short *smp = amalloc((n + 24 * 64) * 2);
    int *a1 = amalloc(n * 4), *a2 = amalloc(n * 4);
    struct mctx x;
    double ts, tv;
    fill(smp, (n + 24 * 64) * 2);
    memset(a1, 0, n * 4); memset(a2, 0, n * 4);
    x.smp = smp; x.n = n;
    x.acc = a1; x.vec = 0; ts = best_us(run_mix, &x);
    x.acc = a2; x.vec = 1; tv = best_us(run_mix, &x);
    /* both ran REPS times: the sums match only if every pass matched */
    line("mix 24 voices x 4096 samples", ts, tv, 24.0 * n * 2, !memcmp(a1, a2, n * 4));
    diff("mix", a1, a2, n * 4);
}

struct xctx { float *out; const float *in; const float *m; size_t n; int vec; };
static void run_xf(void *c)
{
    struct xctx *x = c;
    if (x->vec) VCALL(v_xform, x->out, x->in, x->m, x->n); else s_xform(x->out, x->in, x->m, x->n);
}

static void bench_xform(void)
{
    size_t n = 8192, i;
    float *in = amalloc(n * 16), *o1 = amalloc(n * 16), *o2 = amalloc(n * 16), *m = amalloc(64);
    struct xctx x;
    double ts, tv, maxrel = 0;
    for (i = 0; i < n * 4; i++) in[i] = (float)((int)(rnd() % 20001) - 10000) / 37.0f;
    for (i = 0; i < 16; i++) m[i] = (float)((int)(rnd() % 2001) - 1000) / 513.0f;
    x.in = in; x.m = m; x.n = n;
    x.out = o1; x.vec = 0; ts = best_us(run_xf, &x);
    x.out = o2; x.vec = 1; tv = best_us(run_xf, &x);
    for (i = 0; i < n * 4; i++) {
        double d = fabs((double)o1[i] - o2[i]) / (fabs(o1[i]) + 1.0);
        if (d > maxrel) maxrel = d;
    }
    line("4x4 float transform, 8192 vertices", ts, tv, (double)n * 16, maxrel < 1e-4);
    printf("%-34s largest relative difference %.2g (fused multiply-add)\n", "", maxrel);
}

static void bench(void)
{
    printf("time base %.0f Hz; best of %d runs each\n", s_tb_hz, REPS);
    bench_copy(16 * 1024);
    bench_copy(256 * 1024);
    bench_copy(4 * 1024 * 1024);
    bench_textures();
    bench_mix();
    bench_xform();
}

/* ---- stress -------------------------------------------------------------- */

static int stress(int seconds, unsigned seed, int yield)
{
    static unsigned pat[31 * 4] __attribute__((aligned(16)));
    unsigned long long end;
    unsigned long chunks = 0;
    int i, r;
    for (i = 0; i < 31 * 4; i++)
        pat[i] = seed * 0x01010101u ^ (unsigned)i * 0x9e3779b9u;
    end = tb() + (unsigned long long)(seconds * s_tb_hz);
    while (tb() < end) {
        r = av_stress(pat, 200000);
        chunks++;
        if (r) {
            printf("stress %u: a vector register CHANGED (chunk %lu, %d turns left)\n", seed, chunks, r - 1);
            return 10;
        }
        if (yield)
            Delay(1);
    }
    printf("stress %u: %lu x 200000 turns, every register kept its value\n", seed, chunks);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "info";
    tb_calibrate();
    if (!strcmp(mode, "info"))
        info();
    else if (!strcmp(mode, "bench"))
        bench();
    else if (!strcmp(mode, "bencha")) {
        s_align = 1;
        printf("vector kernels on a 16-byte aligned stack\n");
        bench();
    }
    else if (!strcmp(mode, "stress"))
        return stress(argc > 2 ? atoi(argv[2]) : 10, argc > 3 ? (unsigned)atoi(argv[3]) : 1,
                      argc > 4 ? atoi(argv[4]) : 0);
    else
        printf("avbench info | bench | bencha | stress seconds seed [yield]\n");
    return 0;
}
