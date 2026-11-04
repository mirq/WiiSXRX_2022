/*
 * wos_gl.c -- opengx's PSX-shaped GL over MiniGL (R200), for GlesGpu on WarpOS.
 *
 * GlesGpu was adapted to the Wii through a fork of opengx (deps/opengx), whose
 * GL does the PSX's work in the GX TEV and blender. This file gives the calls
 * GlesGpu makes (WarpOS/wos_gl.h) the same meaning in fixed-function OpenGL:
 *
 * Textures. A PSX texel arrives as 4 bytes, the BGR555 + STP halfword in
 * bytes 2-3. Each texture has two MiniGL textures: the opaque plane and,
 * when an upload with blending on had texels with STP clear, the
 * semi-transparent plane ("semi"), which holds exactly those texels. A
 * texel of 0 is transparent in both. Uploads return which planes got texels
 * (TEX_TYPE_1 semi, TEX_TYPE_2 opaque), as opengx's do.
 *
 * Primitives. With blending off a textured primitive draws once, from the
 * opaque plane. With blending on it draws twice: the semi plane with plain
 * alpha blending (the texels that stay opaque in a semi-transparent
 * primitive), then the opaque plane with the PSX mode (ABR 1..4):
 *      1  0.5B + 0.5F   ONE, SRC_ALPHA with alpha 0.5
 *      2  B + F         ONE, ONE
 *      3  B - F         ONE, ONE, reverse subtract
 *      4  B + 0.25F     ONE, ONE with F at a quarter
 * The colour is the TEV's: texture x vertex colour x 2 (the PSX's 0x80 is
 * 1.0), the ABR factor folded into the vertex colour.
 *
 * MiniGL may lack two things the R200 can do: the texture stage's 2x scale
 * and the reverse-subtract blend. With them (the GL_MGL_texture_env_rgb_scale
 * and GL_EXT_blend_subtract extensions) the result is exact; without, the
 * colour is clamped at 1.0 before the texture multiply (brighter-than-0x80
 * lighting is lost) and B - F becomes B x (1 - F), as Pete's OpenGL plugin did.
 *
 * Every primitive pass calls ogx_draw_submitted_cb, GlesGpu's VRAM readback
 * bookkeeping.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wos_sys.h"
#include "wos_video.h"
#include "../Gamecube/wiiSXconfig.h"

#pragma pack(push, 2)
#include <exec/types.h>
#pragma pack(pop)
#include <proto/minigl.h>

/* Not in MiniGL's header unless the DLL has the extension. */
#ifndef GL_RGB_SCALE
#define GL_RGB_SCALE 0x8573
#endif
#ifndef GL_FUNC_REVERSE_SUBTRACT
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x8006
#endif

#define TEX_TYPE_1 0x1      /* the semi plane got texels */
#define TEX_TYPE_2 0x2      /* the opaque plane got texels */

#define MAX_TEX 2048

/* Planes of another size the texture had before. GlesGpu reuses one name
 * for differently sized uploads (the movie's 256- and 64-wide tiles), and a
 * glTexImage2D costs MiniGL about 90 ms: a size seen again is filled with
 * glTexSubImage2D instead. */
#define TEX_SPARES 3
typedef struct {
    int    w, h, fmt;
    int    pw, ph;          /* the planes' real size (see FMT_565) */
    GLuint main, semi;
} WosPlanes;

/* A plane's texel format. Movies (24-bit displays) have no transparency and
 * go up as RGB565, in planes padded to a power of two: only a power-of-two
 * texture with a retained shadow takes MiniGL's direct subimage path (a
 * 256x208 tile cost ~130 ms on the shared path, which copies on the 68k). The
 * draws scale the texture coordinates by the used part. */
enum { FMT_BGRA, FMT_565 };

typedef struct {
    int    used;
    GLuint main, semi;      /* MiniGL names; 0 = not created */
    GLuint one;             /* both kinds of texel, alpha 128 where STP is set:
                             * an ABR 1 primitive draws once from it */
    int    w, h, fmt;
    int    pw, ph;          /* the planes' real size: w, h but for padded 565 */
    GLint  wraps, wrapt;
    WosPlanes spare[TEX_SPARES];  /* [0] the most recent */
    int    cell;            /* 1..ATLAS_CELLS: a cell of the atlas, 0: own planes */
    int    pal;             /* a cell of the palette-index atlas (wosgl_gp_*) */
} WosTex;

/* GlesGpu's sorted texture cache spreads its sub-textures over 256 x 256
 * pages, each a GL texture: every draw on another page was a texture switch,
 * and with it a new MiniGL record (some 250 a frame in Crash's long views).
 * The pages live as cells of one 2048 x 512 texture instead (MiniGL's
 * partial-update path takes up to that size; with 8 cells of 1024 x 512,
 * GlesGpu's other 8 pages were textures of their own, and a Tekken 3 fight
 * switched between them and the atlas 70-110 times a frame); the draws move their texture
 * coordinates into the cell. The pages clamp (GlesGpu's iClampType) and
 * their sub-textures already sit edge to edge inside a page, so the cells
 * add no new kind of edge. Anything but filling a page piece by piece
 * (a whole new image, a framebuffer copy, a wrap mode) gives the texture
 * its own planes again. SetEnv WSX_ATLAS 0: no atlas. */
#define ATLAS_W 2048
#define ATLAS_H 512
#define ATLAS_CELLS 16
static struct {
    int    state;           /* 0 not yet, 1 made, -1 off */
    GLuint main, semi, one;
    unsigned char used[ATLAS_CELLS];
} s_atlas;
static int s_border_ok;      /* the atlas planes sample white outside s 0..1 */
static void wosgl_state_lost(void);
unsigned int wos_untex_merged;
unsigned int wos_untex_why[6];
/* Palette textures looked up on the GPU (GL_MGL_palette_lookup, see
 * wosgl_gp_init): GlesGpu's 4- and 8-bit texture pages as palette indices
 * in the 16 cells of one 2048 x 512 texture (unit 0), the palettes as rows
 * of a 512 x 512 one (unit 1); the R200's fragment shader reads the index
 * and the palette row (the vertex alpha) and fetches the colour, so these
 * textures need no texture cache, conversion or upload per palette. */
#ifndef MGL_PALETTE_LOOKUP
#define MGL_PALETTE_LOOKUP 0x7021       /* GL_MGL_palette_lookup */
#endif
#define GP_CELLS 16
#define GP_SLOTS 256
static struct {
    int    state;               /* 0 not yet, 1 on, -1 off */
    GLuint idx, pal;            /* MiniGL textures */
    GLuint name[GP_CELLS];      /* wos_gl names of the cells (GlesGpu binds them) */
    short  col[257];            /* the palette column index v lands on; [256] = 512 */
    short  rlo[GP_SLOTS], rhi[GP_SLOTS];    /* the rows slot k writes */
    int    nslots;
    float  rowc;                /* the current primitive's row coordinate */
    int    gl_on;               /* MGL_PALETTE_LOOKUP and unit 1 enabled */
    unsigned uploads[2], texels[2], fails[2];
} s_gp;
/* The cache atlas's width: 1024 (8 cells) while the palette lookup takes
 * the 4- and 8-bit textures (wosgl_gp_init), so the two fit the PPC's
 * 64 MiB VRAM aperture with MiniGL's 3 copies of each; else ATLAS_W. */
static int s_atlas_w = ATLAS_W;
#define CELL_X(c) ((((c) - 1) % (s_atlas_w / 256)) * 256)
#define CELL_Y(c) ((((c) - 1) / (s_atlas_w / 256)) * 256)
/* the palette-index atlas: 16 cells of 2048 x 512 */
#define GP_W 2048
#define GP_H 512
#define GP_CELL_X(c) ((((c) - 1) % (GP_W / 256)) * 256)
#define GP_CELL_Y(c) ((((c) - 1) / (GP_W / 256)) * 256)

static WosTex s_tex[MAX_TEX];
static int    s_cur;                    /* the bound (opengx) texture */

static int    s_texture_enabled;
static int    s_has_ascale;             /* GL_MGL_texture_env_alpha_scale in use */
static int    s_has_unsync;             /* GL_MGL_unsync_subimage */
static int    s_blend_enabled;
static int    s_abr;                    /* glSetGlobalTextABR */
static int    s_rgb24;
static int    s_no_need_mul;            /* the colour is white: texture alone */
static int    s_tex_semi_type;          /* glSetTextureType: planes of the bound texture */
static GLenum s_zfunc = GL_LESS;
static GLubyte s_lcol[4];               /* glColor4Lcol: R G B A */
static GLfloat s_clear[4];

static int    s_has_scale2;             /* MiniGL can scale the texture stage by 2 */
static int    s_has_rsub;               /* MiniGL can blend B - F */
static int    s_state_ok;
static GLint  s_scissor[4];
/* The projection and viewport, for the draws that map their vertices to
 * the screen themselves (see draw): an ortho on identity matrices only. */
static int    s_proj_state;             /* 0 unknown, 1 identity, 2 the ortho below */
static int    s_mv_identity = 1;
static float  s_ortho[6];               /* left right bottom top near far */
static GLint  s_vp[4];
static int    s_scissor_on;

void (*ogx_draw_submitted_cb)(void);

/* The texel buffers: one texture's worth (up to the atlas, BGRA) per plane. */
static GLubyte *s_conv_main, *s_conv_semi, *s_conv_one;
static int      s_conv_px;

/* ---- Set-up ------------------------------------------------------------ */

static int has_ext(const char *name)
{
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    return ext && strstr(ext, name) != NULL;
}

void ogx_initialize(void)
{
    if (s_state_ok)
        return;
    s_has_scale2 = has_ext("GL_MGL_texture_env_rgb_scale") && !wos_env_int("WSX_GL_NOSCALE", 0);
    s_has_rsub   = has_ext("GL_EXT_blend_subtract") && !wos_env_int("WSX_GL_NORSUB", 0);
    s_has_ascale = s_has_scale2 && has_ext("GL_MGL_texture_env_alpha_scale") &&
                   !wos_env_int("WSX_GL_NOASCALE", 0);
    s_has_unsync = has_ext("GL_MGL_unsync_subimage") && wos_env_int("WSX_UNSYNC", 1);
    wos_log("[gl] MiniGL: 2x texture scale %s, reverse-subtract blend %s, 2x alpha scale %s, unsynced sub-images %s",
            s_has_scale2 ? "yes" : "no (colours clamp at 1.0)",
            s_has_rsub ? "yes" : "no (B-F approximated)",
            s_has_ascale ? "yes" : "no",
            s_has_unsync ? "yes" : "no");
    memset(s_tex, 0, sizeof(s_tex));
    glEnable(GL_SCISSOR_TEST);
    s_scissor_on = 1;
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    s_state_ok = 1;
}

static int ensure_conv(int px)
{
    if (px <= s_conv_px)
        return 1;
    free(s_conv_main);
    free(s_conv_semi);
    free(s_conv_one);
    s_conv_main = (GLubyte *)malloc((size_t)px * 4);
    s_conv_semi = (GLubyte *)malloc((size_t)px * 4);
    s_conv_one = (GLubyte *)malloc((size_t)px * 4);
    if (!s_conv_main || !s_conv_semi || !s_conv_one) {
        free(s_conv_main); free(s_conv_semi); free(s_conv_one);
        s_conv_main = s_conv_semi = s_conv_one = NULL;
        s_conv_px = 0;
        return 0;
    }
    s_conv_px = px;
    return 1;
}

/* ---- Textures ---------------------------------------------------------- */

static GLint filter_for(void)
{
    if (originalMode == ORIGINALMODE_ENABLE || bilinearFilter != BILINEARFILTER_ENABLE)
        return GL_NEAREST;
    return GL_LINEAR;
}

static GLint wrap_conv(GLint p)
{
    switch (p) {
    case GL_CLAMP:
    case 0x812F: /* GL_CLAMP_TO_EDGE */
        return GL_CLAMP;
    default:
        return GL_REPEAT;
    }
}

/* MiniGL textures made and deleted here, for the log (a leak fills VRAM). */
unsigned int wos_gl_tex_made, wos_gl_tex_deleted;
static void gl_del(GLuint *name)
{
    glDeleteTextures(1, name);
    wos_gl_tex_deleted++;
}

/* A MiniGL texture of w x h, its texels from bgra (or black). */
unsigned int wos_tex_subs, wos_tex_texels, wos_draws;    /* partial uploads, for the log */
unsigned int wos_tex_unsync;            /* of them with a sync point */
/* texels sent by kind: 1 whole image, 2 cache upload, 3 deferred flush,
 * 4 cell clear, 5 movie, 6 framebuffer copy, 7 other partial */
unsigned int wos_up_texels[8], wos_up_calls[8];
unsigned int wos_twin_n;
unsigned int wos_texpair[5][5];
uint64_t wos_up_ticks[8];               /* time in glTexSubImage2D/glTexImage2D by kind */
static int s_upk;
#define WOS_UPB(k, n) (wos_up_texels[k] += (unsigned)(n), wos_up_calls[k]++, s_upk = (k))
#define WOS_PROFUP(stmt) do { uint32_t _t0 = wos_tb_lo(); stmt; _t0 = wos_tb_lo() - _t0; \
        wos_prof_ticks[WOS_PROF_TEXUP] += _t0; wos_up_ticks[s_upk] += _t0; } while (0)

/* GL_MGL_unsync_subimage. MiniGL updates a texture the GPU may still read
 * by copying it to a spare surface, and waits until the GPU is done with
 * the one it picks: with Tekken 3's ~11 cache uploads a frame, ~1.7 ms
 * each. GlesGpu's cache uploads go to free space of its page, which no
 * draw issued since the space was freed samples: it notes MiniGL's sync
 * point when it frees space (wosgl_sync_point) and hands the page's point
 * to the upload (wosgl_unsync_after); once that point has retired MiniGL
 * need not wait. SetEnv WSX_UNSYNC 0: always wait, as before. */
unsigned int wosgl_unsync_after;

unsigned int wosgl_sync_point(void)
{
    GLint v = 0;
    if (!s_has_unsync)
        return 0;
    glGetIntegerv(MGL_SYNC_POINT, &v);
    return (unsigned int)v;
}
static GLuint make_plane_fmt(int w, int h, GLint wraps, GLint wrapt, const GLubyte *bgra, int fmt);
static GLuint make_plane(int w, int h, GLint wraps, GLint wrapt, const GLubyte *bgra)
{
    return make_plane_fmt(w, h, wraps, wrapt, bgra, FMT_BGRA);
}

static GLuint make_plane_fmt(int w, int h, GLint wraps, GLint wrapt, const GLubyte *bgra, int fmt)
{ wosgl_gl_touched();
    GLuint name = 0;
    GLint f = filter_for();
    glGenTextures(1, &name);
    wos_gl_tex_made++;
    glBindTexture(GL_TEXTURE_2D, name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wraps);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapt);
    if (!bgra) {
        if (!ensure_conv(w * h))
            return name;
        memset(s_conv_semi, 0, (size_t)w * h * 4);
        bgra = s_conv_semi;
    }
    WOS_CRUMB2(87, (w << 16) | h);
    { extern unsigned int wos_up_texels[8], wos_up_calls[8];
      wos_up_texels[0] += (unsigned)(w * h); wos_up_calls[0]++; s_upk = 0; }
    if (fmt == FMT_565)
        WOS_PROFUP( glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, MGL_UNSIGNED_SHORT_5_6_5, bgra));
    else
        WOS_PROFUP( glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, bgra));
    WOS_CRUMB(88);
    wos_prof_reads += 0x10000;      /* texture creations, in the high half */
    return name;
}

/* The one plane is kept only by uploads that split the texels; anything
 * else that writes the planes drops it (the primitives draw twice again). */
static void drop_one(WosTex *t)
{
    if (t->one) {
        wosgl_gl_touched();
        gl_del(&t->one);
        t->one = 0;
    }
}

/* Leave the atlas: the planes are the atlas's, not the texture's. */
static void detach_cell(WosTex *t)
{
    if (!t->cell || t->pal)     /* a palette-index cell is wos_gl's own */
        return;
    wosgl_gl_touched();
    s_atlas.used[t->cell - 1] = 0;
    t->cell = 0;
    t->main = t->semi = t->one = 0;
    t->w = t->h = t->pw = t->ph = 0;
}

static void free_planes(WosTex *t)
{ wosgl_gl_touched();
    int i;
    detach_cell(t);
    drop_one(t);
    if (t->main) gl_del(&t->main);
    if (t->semi) gl_del(&t->semi);
    t->main = t->semi = 0;
    t->w = t->h = 0;
    for (i = 0; i < TEX_SPARES; i++) {
        if (t->spare[i].main) gl_del(&t->spare[i].main);
        if (t->spare[i].semi) gl_del(&t->spare[i].semi);
    }
    memset(t->spare, 0, sizeof t->spare);
}

static void set_plane_params(GLuint name, WosTex *t)
{ wosgl_gl_touched();
    GLint f = filter_for();
    glBindTexture(GL_TEXTURE_2D, name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, t->wraps);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, t->wrapt);
}

/* Make the planes w x h: the spare of that size if there is one (1), else
 * none yet (0) -- the caller creates them. The old planes become a spare. */
static int switch_planes(WosTex *t, int w, int h, int fmt)
{ wosgl_gl_touched();
    WosPlanes cur, found;
    int i, k = -1;

    if (t->main && t->w == w && t->h == h && t->fmt == fmt && !t->cell)
        return 1;
    detach_cell(t);
    drop_one(t);
    for (i = 0; i < TEX_SPARES; i++)
        if (t->spare[i].main && t->spare[i].w == w && t->spare[i].h == h && t->spare[i].fmt == fmt) {
            k = i;
            break;
        }
    cur.w = t->w; cur.h = t->h; cur.fmt = t->fmt; cur.main = t->main; cur.semi = t->semi;
    cur.pw = t->pw; cur.ph = t->ph;
    memset(&found, 0, sizeof found);
    if (k >= 0) {
        found = t->spare[k];
        for (i = k; i < TEX_SPARES - 1; i++)
            t->spare[i] = t->spare[i + 1];
        memset(&t->spare[TEX_SPARES - 1], 0, sizeof t->spare[0]);
    }
    if (cur.main) {
        WosPlanes *last = &t->spare[TEX_SPARES - 1];
        if (last->main) gl_del(&last->main);
        if (last->semi) gl_del(&last->semi);
        memmove(&t->spare[1], &t->spare[0], sizeof t->spare[0] * (TEX_SPARES - 1));
        t->spare[0] = cur;
    }
    t->main = found.main;
    t->semi = found.semi;
    t->w = k >= 0 ? w : 0;
    t->h = k >= 0 ? h : 0;
    t->pw = k >= 0 ? found.pw : 0;
    t->ph = k >= 0 ? found.ph : 0;
    t->fmt = fmt;
    if (k < 0)
        return 0;
    set_plane_params(t->main, t);
    if (t->semi)
        set_plane_params(t->semi, t);
    return 1;
}

static inline void put_bgra(GLubyte *d, unsigned v, GLubyte a)
{
    unsigned r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
    d[0] = (GLubyte)((b << 3) | (b >> 2));
    d[1] = (GLubyte)((g << 3) | (g >> 2));
    d[2] = (GLubyte)((r << 3) | (r >> 2));
    d[3] = a;
}

/* PSX texels (4 bytes, the halfword in bytes 2-3, `pitch` texels a row) into
 * the two planes' BGRA; returns the TEX_TYPE_* mask. */
static int split_texels(const GLubyte *src, int pitch, int w, int h, int rgb24)
{
    int x, y, type = 0;
    GLubyte *m = s_conv_main, *s = s_conv_semi, *o = s_conv_one;

    for (y = 0; y < h; y++) {
        const GLubyte *p = src + (size_t)y * pitch * 4;
        for (x = 0; x < w; x++, p += 4, m += 4, s += 4, o += 4) {
            unsigned v = ((unsigned)p[2] << 8) | p[3];
            if (rgb24) {                            /* movies: one plane */
                put_bgra(m, v, (v & 0x8000) ? 255 : 0);
                memset(s, 0, 4);
                type |= TEX_TYPE_2;
            } else if (v == 0) {
                memset(m, 0, 4);
                memset(s, 0, 4);
                memset(o, 0, 4);
            } else if (s_blend_enabled && !(v & 0x8000)) {
                put_bgra(s, v, 255);
                put_bgra(o, v, 255);
                memset(m, 0, 4);
                type |= TEX_TYPE_1;
            } else {
                put_bgra(m, v, 255);
                put_bgra(o, v, 128);
                memset(s, 0, 4);
                type |= TEX_TYPE_2;
            }
        }
    }
    return type;
}

void wosgl_GenTextures(GLsizei n, GLuint *textures)
{
    int i;
    for (i = 1; i < MAX_TEX && n > 0; i++) {
        if (!s_tex[i].used) {
            memset(&s_tex[i], 0, sizeof(s_tex[i]));
            s_tex[i].used = 1;
            s_tex[i].wraps = s_tex[i].wrapt = GL_REPEAT;
            *textures++ = (GLuint)i;
            n--;
        }
    }
    while (n-- > 0)
        *textures++ = 0;
}

void wosgl_DeleteTextures(GLsizei n, const GLuint *textures)
{ wosgl_gl_touched();
    while (n-- > 0) {
        GLuint i = *textures++;
        if (i > 0 && i < MAX_TEX && s_tex[i].used) {
            free_planes(&s_tex[i]);
            s_tex[i].used = 0;
        }
    }
}

void wosgl_BindTextureBef(GLenum target, GLuint texture)
{
    (void)target;
    if (texture < MAX_TEX)
        s_cur = (int)texture;
}

void wosgl_TexParameteri(GLenum target, GLenum pname, GLint param)
{ wosgl_gl_touched();
    WosTex *t = &s_tex[s_cur];
    if (target != GL_TEXTURE_2D || !s_cur || t->pal)
        return;
    /* opengx keeps only the wrap modes; the filter follows the settings. */
    if (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T) {
        GLint w = wrap_conv(param);
        if (pname == GL_TEXTURE_WRAP_S) t->wraps = w; else t->wrapt = w;
        if (t->cell) {
            if (w != GL_CLAMP)
                detach_cell(t);     /* it repeats: its texels are lost, as with a new size */
            return;
        }
        if (t->main) {
            glBindTexture(GL_TEXTURE_2D, t->main);
            glTexParameteri(GL_TEXTURE_2D, pname, w);
        }
        if (t->semi) {
            glBindTexture(GL_TEXTURE_2D, t->semi);
            glTexParameteri(GL_TEXTURE_2D, pname, w);
        }
        if (t->one) {
            glBindTexture(GL_TEXTURE_2D, t->one);
            glTexParameteri(GL_TEXTURE_2D, pname, w);
        }
    }
}

void wosgl_TexParameterf(GLenum target, GLenum pname, GLfloat param)
{ wosgl_gl_touched();
    wosgl_TexParameteri(target, pname, (GLint)param);
}

/* Calls that only set flags (or GL state the cached key does not cover:
 * scissor, viewport, matrices) keep the cache: forgetting it made every
 * following draw re-send its whole state, some 170 times a frame. */
void wosgl_TexEnvf(GLenum target, GLenum pname, GLfloat param)
{
    (void)target; (void)pname; (void)param;   /* the draws set the stage */
}

/* GlesGpu's 1555 texels (in the low half of each word) as RGB565. */
static void to_565(const GLubyte *src, int pitch, int w, int h)
{
    uint16_t *d = (uint16_t *)s_conv_main;
    int x, y;
    for (y = 0; y < h; y++) {
        const GLubyte *p = src + (size_t)y * pitch * 4;
        for (x = 0; x < w; x++, p += 4) {
            unsigned v = ((unsigned)p[2] << 8) | p[3];
            unsigned r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
            *d++ = (uint16_t)((r << 11) | (g << 6) | ((g >> 4) << 5) | b);
        }
    }
}

/* Replace the bound texture's planes with w x h texels. */
static int define_texture(int w, int h, const void *data, int rgb24)
{ wosgl_gl_touched();
    WosTex *t = &s_tex[s_cur];
    int type;

    detach_cell(t);
    if (!s_cur || !t->used || w <= 0 || h <= 0 || !ensure_conv(w * h))
        return 0;
    if (rgb24) {
        WOS_PROF(WOS_PROF_TEXCONV, to_565((const GLubyte *)data, w, w, h));
        if (!switch_planes(t, w, h, FMT_565)) {
            int pw = 1, ph = 1;
            while (pw < w) pw <<= 1;
            while (ph < h) ph <<= 1;
            /* Black, then one full-image subimage: that starts the shadow
             * the direct path needs. The texels follow below. */
            if (!ensure_conv(pw * ph))
                return 0;
            memset(s_conv_semi, 0, (size_t)pw * ph * 2);
            t->main = make_plane_fmt(pw, ph, t->wraps, t->wrapt, s_conv_semi, FMT_565);
            WOS_UPB(5, (pw) * (ph)); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pw, ph, GL_RGB, MGL_UNSIGNED_SHORT_5_6_5, s_conv_semi);
            t->w = w;
            t->h = h;
            t->pw = pw;
            t->ph = ph;
            WOS_PROF(WOS_PROF_TEXCONV, to_565((const GLubyte *)data, w, w, h));
        }
        glBindTexture(GL_TEXTURE_2D, t->main);
        WOS_UPB(5, (w) * (h)); WOS_PROFUP( glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, MGL_UNSIGNED_SHORT_5_6_5, s_conv_main));
        return TEX_TYPE_2;
    }
    WOS_PROF(WOS_PROF_TEXCONV, type = split_texels((const GLubyte *)data, w, w, h, rgb24));

    if (!switch_planes(t, w, h, FMT_BGRA)) {
        t->main = make_plane(w, h, t->wraps, t->wrapt, s_conv_main);
        t->w = t->pw = w;
        t->h = t->ph = h;
    } else {
        glBindTexture(GL_TEXTURE_2D, t->main);
        WOS_UPB(1, (w) * (h)); WOS_PROFUP( glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_main));
    }
    if (type & TEX_TYPE_1) {
        if (!t->semi)
            t->semi = make_plane(w, h, t->wraps, t->wrapt, s_conv_semi);
        else {
            glBindTexture(GL_TEXTURE_2D, t->semi);
            WOS_UPB(1, (w) * (h)); WOS_PROFUP( glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_semi));
        }
    }
    if ((type & (TEX_TYPE_1 | TEX_TYPE_2)) == (TEX_TYPE_1 | TEX_TYPE_2) || t->one) {
        if (!t->one)
            t->one = make_plane(w, h, t->wraps, t->wrapt, s_conv_one);
        else {
            glBindTexture(GL_TEXTURE_2D, t->one);
            WOS_UPB(1, (w) * (h)); WOS_PROFUP( glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_one));
        }
    }
    return type;
}

int wosgl_TexImage2D(GLenum target, GLint level, GLint internalFormat,
                     GLsizei width, GLsizei height, GLint border,
                     GLenum format, GLenum type, const GLvoid *data)
{ wosgl_gl_touched();
    (void)level; (void)internalFormat; (void)border; (void)format; (void)type;
    if (target != GL_TEXTURE_2D || !data || s_tex[s_cur].pal)
        return 0;
    return define_texture(width, height, data, 0);
}

static int atlas_defer(int ux, int uy, int cw, int ch, unsigned after);
static GLubyte *s_atl_copy[2];
static int s_defer;
static void atlas_flush(int k);

int wosgl_TexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                        GLsizei width, GLsizei height, GLenum format, GLenum type,
                        const GLvoid *data)
{ wosgl_gl_touched();
    WosTex *t = &s_tex[s_cur];
    int tt;
    (void)level; (void)format; (void)type;

    if (target != GL_TEXTURE_2D || !s_cur || !t->main || !data)
        return 0;
    if (xoffset < 0 || yoffset < 0 || xoffset + width > t->w || yoffset + height > t->h) {
        if (xoffset >= t->w || yoffset >= t->h)
            return 0;
        /* clipped as opengx does: the source rows keep their full width */
    }
    {
        int cw = width, ch = height;
        if (xoffset + cw > t->w) cw = t->w - xoffset;
        if (yoffset + ch > t->h) ch = t->h - yoffset;
        if (cw <= 0 || ch <= 0 || !ensure_conv(cw * ch))
            return 0;
        if (t->fmt == FMT_565) {
            WOS_PROF(WOS_PROF_TEXCONV, to_565((const GLubyte *)data, width, cw, ch));
            glBindTexture(GL_TEXTURE_2D, t->main);
            WOS_UPB(7, (cw) * (ch)); WOS_PROFUP( glTexSubImage2D(GL_TEXTURE_2D, 0, xoffset, yoffset, cw, ch,
                            GL_RGB, MGL_UNSIGNED_SHORT_5_6_5, s_conv_main));
            return TEX_TYPE_2;
        }
        wos_tex_subs++;
        wos_tex_texels += (unsigned)(cw * ch);
        {   /* SetEnv WSX_TEXLOG n: the first n cache uploads after frame 1500 */
            extern int framesdone;
            static int left = -1;
            if (left < 0)
                left = wos_env_int("WSX_TEXLOG", 0);
            if (left > 0 && framesdone > 1500) {
                left--;
                wos_log("[texlog] frame %d tex %d cell %d at %d,%d %dx%d (rows of %d) one %d",
                        framesdone, s_cur, t->cell, xoffset, yoffset, cw, ch, width, t->one ? 1 : 0);
            }
        }
        WOS_PROF(WOS_PROF_TEXCONV, tt = split_texels((const GLubyte *)data, width, cw, ch, 0));
        {
            int ux = xoffset + (t->cell ? CELL_X(t->cell) : 0);
            int uy = yoffset + (t->cell ? CELL_Y(t->cell) : 0);
            unsigned int after = s_has_unsync ? wosgl_unsync_after : 0;
            int deferred;
            if (t->cell && !after) {
                extern unsigned int wos_up_texels[8], wos_up_calls[8];
                static int nlog = 8;
                wos_up_calls[7]++;          /* "other": a cache upload without a sync point */
                if (nlog > 0) {
                    nlog--;
                    wos_log("[texlog] no sync point: tex %d cell %d at %d,%d %dx%d", s_cur, t->cell, xoffset, yoffset, cw, ch);
                }
            }
            if (after) {
                glPixelStorei(MGL_UNPACK_UNSYNC_AFTER, (GLint)after);
                wos_tex_unsync++;
            }
            deferred = t->cell && t->one && t->main == s_atlas.main && t->semi == s_atlas.semi &&
                       atlas_defer(ux, uy, cw, ch, after);
            WOS_CRUMB2(85, (xoffset << 16) | yoffset);
            if (!deferred) {
                glBindTexture(GL_TEXTURE_2D, t->main);
                WOS_UPB(2, (cw) * (ch)); WOS_PROFUP( glTexSubImage2D(GL_TEXTURE_2D, 0, ux, uy, cw, ch,
                                MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_main));
            }
            WOS_CRUMB2(86, (cw << 16) | ch);
            if ((tt & TEX_TYPE_1) && !t->semi)
                t->semi = make_plane(t->w, t->h, t->wraps, t->wrapt, NULL);
            if (t->semi && !deferred) {
                glBindTexture(GL_TEXTURE_2D, t->semi);
                WOS_UPB(2, (cw) * (ch)); WOS_PROFUP( glTexSubImage2D(GL_TEXTURE_2D, 0, ux, uy, cw, ch,
                                MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_semi));
            }
            /* A one plane made before has the rest of the texels; a new one
             * would not, so only an existing one follows a partial upload. */
            if (t->one) {
                glBindTexture(GL_TEXTURE_2D, t->one);
                WOS_UPB(2, (cw) * (ch)); WOS_PROFUP( glTexSubImage2D(GL_TEXTURE_2D, 0, ux, uy, cw, ch,
                                MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_one));
            }
            if (after)
                glPixelStorei(MGL_UNPACK_UNSYNC_AFTER, 0);
        }
    }
    return tt;
}

/* The atlas's main and semi planes, written when a draw samples them.
 * With MiniGL's alpha x 2 (s_has_ascale) and the one plane, opaque and
 * ABR 1 primitives draw from the one plane alone; only ABR 0/2/3 sample
 * main and semi (Tekken 3's fights: ~15 of ~215 blended primitives a
 * frame). A cache upload then crosses the Zorro bus once instead of three
 * times: the one plane goes up at once, the main and semi texels into a
 * copy here, and their rectangles to MiniGL before the next draw that
 * samples that plane (compute_state). SetEnv WSX_GLDEFER 0: all three at
 * once, as before. */
typedef struct { short x, y, w, h; unsigned after; } WosRect;
#define DEFER_MAX 64
static int s_defer = -1;
/* s_atl_copy (above): [0] main, [1] semi: ATLAS_W x ATLAS_H BGRA */
static GLubyte *s_defer_buf;            /* one rectangle, packed */
static WosRect s_pend[2][DEFER_MAX];
static int s_npend[2];
unsigned int wos_defer_rects, wos_defer_flushes;   /* for the log */

void wosgl_gl_touched(void);

/* The waiting rectangles of plane k inside atlas cell `cell` (0: all) go up;
 * the others stay: a draw samples only its own texture's cell. */
static void atlas_flush_cell(int k, int cell)
{
    int i, j, y, any = 0;
    GLuint tex = k ? s_atlas.semi : s_atlas.main;
    int cx = cell ? CELL_X(cell) : 0, cy = cell ? CELL_Y(cell) : 0;

    for (i = 0; i < s_npend[k] && !any; i++)
        if (!cell || (s_pend[k][i].x >= cx && s_pend[k][i].x < cx + 256 &&
                      s_pend[k][i].y >= cy && s_pend[k][i].y < cy + 256))
            any = 1;
    if (!any)
        return;
    wosgl_gl_touched();                 /* the open batch first; the binding changes */
    wos_defer_flushes++;
    glBindTexture(GL_TEXTURE_2D, tex);
    {
        /* The rectangles of the cell, neighbours merged into their bounding
         * box while it adds few texels: a call costs ~65 us besides its
         * ~0.12 us a texel (Tekken 3: ~19 rectangles a flush). The copy
         * mirrors the plane, so texels between them go up unchanged. The
         * box's sync point is the latest of its rectangles' (none if one
         * has none). SetEnv WSX_DEFERMERGE 0: one call a rectangle. */
        static int merge = -1, slack;
        WosRect m;
        int have = 0, area = 0;
        if (merge < 0) {
            merge = wos_env_int("WSX_DEFERMERGE", 1);
            slack = wos_env_int("WSX_DEFERSLACK", 512);
        }
        for (i = j = 0; i <= s_npend[k]; i++) {
            const WosRect *r = i < s_npend[k] ? &s_pend[k][i] : NULL;
            if (r && cell && !(r->x >= cx && r->x < cx + 256 && r->y >= cy && r->y < cy + 256)) {
                s_pend[k][j++] = *r;    /* another cell's: stays */
                continue;
            }
            if (r && have && merge) {
                int x0 = r->x < m.x ? r->x : m.x, y0 = r->y < m.y ? r->y : m.y;
                int x1 = r->x + r->w > m.x + m.w ? r->x + r->w : m.x + m.w;
                int y1 = r->y + r->h > m.y + m.h ? r->y + r->h : m.y + m.h;
                /* same cell only: a box across cells would cover another
                 * cell's waiting rectangles out of order */
                if ((x1 - x0) * (y1 - y0) <= area + r->w * r->h + slack &&
                    (x0 >> 8) == ((x1 - 1) >> 8) && (y0 >> 8) == ((y1 - 1) >> 8)) {
                    m.x = (short)x0; m.y = (short)y0;
                    m.w = (short)(x1 - x0); m.h = (short)(y1 - y0);
                    area += r->w * r->h;
                    if (!m.after || !r->after)
                        m.after = 0;
                    else if ((int)(r->after - m.after) > 0)
                        m.after = r->after;
                    continue;
                }
            }
            if (have) {
                for (y = 0; y < m.h; y++)
                    memcpy(s_defer_buf + (size_t)y * m.w * 4,
                           s_atl_copy[k] + ((size_t)(m.y + y) * s_atlas_w + m.x) * 4,
                           (size_t)m.w * 4);
                if (s_has_unsync && m.after)
                    glPixelStorei(MGL_UNPACK_UNSYNC_AFTER, (GLint)m.after);
                WOS_UPB(3, (m.w) * (m.h)); WOS_PROFUP( glTexSubImage2D(GL_TEXTURE_2D, 0, m.x, m.y, m.w, m.h,
                                MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_defer_buf));
                if (s_has_unsync && m.after)
                    glPixelStorei(MGL_UNPACK_UNSYNC_AFTER, 0);
                have = 0;
            }
            if (r) {
                m = *r;
                area = r->w * r->h;
                have = 1;
            }
        }
    }
    s_npend[k] = j;
}

static void atlas_flush(int k)
{
    atlas_flush_cell(k, 0);
}

static int defer_on(void)
{
    if (s_defer < 0) {
        s_defer = s_has_ascale && wos_env_int("WSX_ONEPASS", 1) && wos_env_int("WSX_GLDEFER", 1);
        if (s_defer) {
            s_atl_copy[0] = (GLubyte *)calloc((size_t)s_atlas_w * ATLAS_H, 4);
            s_atl_copy[1] = (GLubyte *)calloc((size_t)s_atlas_w * ATLAS_H, 4);
            s_defer_buf = (GLubyte *)malloc(256 * 256 * 4);
            if (!s_atl_copy[0] || !s_atl_copy[1] || !s_defer_buf)
                s_defer = 0;
        }
        wos_log("[gl] main/semi atlas planes %s", s_defer ? "written when sampled" : "written at once");
    }
    return s_defer;
}

/* A cache upload's main and semi texels (the conversion buffers, cw x ch)
 * for later; 0 when they must go up now. */
static int atlas_defer(int ux, int uy, int cw, int ch, unsigned after)
{
    int k, y;
    if (!defer_on() || cw > 256 || ch > 256)
        return 0;
    for (k = 0; k < 2; k++) {
        const GLubyte *src = k ? s_conv_semi : s_conv_main;
        WosRect *r;
        if (s_npend[k] == DEFER_MAX)
            atlas_flush(k);
        for (y = 0; y < ch; y++)
            memcpy(s_atl_copy[k] + ((size_t)(uy + y) * s_atlas_w + ux) * 4,
                   src + (size_t)y * cw * 4, (size_t)cw * 4);
        r = &s_pend[k][s_npend[k]++];
        r->x = (short)ux; r->y = (short)uy; r->w = (short)cw; r->h = (short)ch;
        r->after = after;
    }
    wos_defer_rects++;
    return 1;
}

/* A free cell of the atlas for the bound texture (made on first use),
 * cleared: the page fills piece by piece and must not show the last one's. */
static int atlas_take(WosTex *t)
{
    int c;
    if (s_atlas.state == 0) {
        s_atlas.state = wos_env_int("WSX_ATLAS", 1) ? 1 : -1;
        if (s_atlas.state > 0) {
            s_atlas.main = make_plane(s_atlas_w, ATLAS_H, GL_CLAMP, GL_CLAMP, NULL);
            s_atlas.semi = make_plane(s_atlas_w, ATLAS_H, GL_CLAMP, GL_CLAMP, NULL);
            s_atlas.one = make_plane(s_atlas_w, ATLAS_H, GL_CLAMP, GL_CLAMP, NULL);
            if (!s_atlas.main || !s_atlas.semi || !s_atlas.one)
                s_atlas.state = -1;
            else if (wos_env_int("WSX_UNTEXMERGE", 0)) {
                /* s clamped to a white border (MiniGL GL_CLAMP_TO_BORDER):
                 * untextured primitives can then join a textured batch,
                 * sampling white outside the plane (draw, s_border_ok) */
                GLuint pl[3];
                int i;
                pl[0] = s_atlas.main; pl[1] = s_atlas.semi; pl[2] = s_atlas.one;
                while (glGetError() != GL_NO_ERROR)
                    ;
                s_border_ok = 1;
                for (i = 0; i < 3; i++) {
                    glBindTexture(GL_TEXTURE_2D, pl[i]);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812D);
                    if (glGetError() != GL_NO_ERROR)
                        s_border_ok = 0;
                }
                if (!s_border_ok)
                    for (i = 0; i < 3; i++) {
                        glBindTexture(GL_TEXTURE_2D, pl[i]);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
                    }
                wosgl_state_lost();     /* the binding moved */
                wos_log("[gl] untextured primitives in textured batches: %s",
                        s_border_ok ? "on (white border)" : "off (no GL_CLAMP_TO_BORDER)");
            }
        }
    }
    if (s_atlas.state < 0 || !ensure_conv(256 * 256))
        return 0;
    {
        static int cells = -1;      /* SetEnv WSX_ATLASCELLS n: fewer cells (tests) */
        if (cells < 0) {
            int most = s_atlas_w / 256 * (ATLAS_H / 256);
            cells = wos_env_int("WSX_ATLASCELLS", most);
            if (cells < 1 || cells > most)
                cells = most;
        }
        for (c = 0; c < cells && s_atlas.used[c]; c++)
            ;
        if (c == cells)
            return 0;
    }
    s_atlas.used[c] = 1;
    t->cell = c + 1;
    t->main = s_atlas.main;
    t->semi = s_atlas.semi;
    t->one = s_atlas.one;
    t->w = t->pw = 256;
    t->h = t->ph = 256;
    t->fmt = FMT_BGRA;
    atlas_flush(0);             /* rectangles of this cell must not land after the clear */
    atlas_flush(1);
    if (s_defer > 0) {          /* the copies mirror the planes (atlas_flush_cell merges) */
        int k, y;
        for (k = 0; k < 2; k++)
            for (y = 0; y < 256; y++)
                memset(s_atl_copy[k] + ((size_t)(CELL_Y(t->cell) + y) * s_atlas_w + CELL_X(t->cell)) * 4,
                       0, 256 * 4);
    }
    memset(s_conv_semi, 0, 256 * 256 * 4);
    glBindTexture(GL_TEXTURE_2D, s_atlas.main);
    WOS_UPB(4, (256) * (256)); glTexSubImage2D(GL_TEXTURE_2D, 0, CELL_X(t->cell), CELL_Y(t->cell), 256, 256, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_semi);
    glBindTexture(GL_TEXTURE_2D, s_atlas.semi);
    WOS_UPB(4, (256) * (256)); glTexSubImage2D(GL_TEXTURE_2D, 0, CELL_X(t->cell), CELL_Y(t->cell), 256, 256, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_semi);
    glBindTexture(GL_TEXTURE_2D, s_atlas.one);
    WOS_UPB(4, (256) * (256)); glTexSubImage2D(GL_TEXTURE_2D, 0, CELL_X(t->cell), CELL_Y(t->cell), 256, 256, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_semi);
    return 1;
}

void wosgl_InitRGBATextures(GLsizei width, GLsizei height)
{ wosgl_gl_touched();
    WosTex *t = &s_tex[s_cur];
    if (!s_cur || !t->used || width <= 0 || height <= 0)
        return;
    if (t->cell && width == 256 && height == 256)
        return;                 /* reused: its texels are replaced before use */
    if (width == 256 && height == 256 && !t->main && atlas_take(t))
        return;
    if (switch_planes(t, width, height, FMT_BGRA))
        return;                 /* reused: its texels are replaced before use */
    t->main = make_plane(width, height, t->wraps, t->wrapt, NULL);
    /* GlesGpu fills these pages (its sub-texture cache) piece by piece:
     * the one plane must exist from the start to follow every piece. */
    t->one = make_plane(width, height, t->wraps, t->wrapt, NULL);
    t->w = t->pw = width;
    t->h = t->ph = height;
}

void wosgl_ResetMovieTexPtr(void) {}

int wosgl_InitMovieTextures(GLsizei width, GLsizei height, void *texData)
{ wosgl_gl_touched();
    return define_texture(width, height, texData, s_rgb24);
}

/* ---- The framebuffer --------------------------------------------------- */

static GLubyte *s_read;
static int      s_read_px;

/* Read w x h from the framebuffer at (x, y), top-left origin, as RGBA rows
 * top first. */
static GLubyte *read_fb(int x, int y, int w, int h)
{ wosgl_gl_touched();
    int fbh = wos_video_height();
    int row;

    if (w <= 0 || h <= 0)
        return NULL;
    if (s_read_px < w * h * 2) {
        free(s_read);
        s_read = (GLubyte *)malloc((size_t)w * h * 2 * 4);
        s_read_px = s_read ? w * h * 2 : 0;
        if (!s_read)
            return NULL;
    }
    /* GL's rows run bottom-up: read into the second half, then flip. */
    wos_prof_reads++;
    WOS_PROF(WOS_PROF_READ, glReadPixels(x, fbh - y - h, w, h, GL_RGBA, GL_UNSIGNED_BYTE, s_read + (size_t)w * h * 4));
    for (row = 0; row < h; row++)
        memcpy(s_read + (size_t)row * w * 4,
               s_read + (size_t)w * h * 4 + (size_t)(h - 1 - row) * w * 4, (size_t)w * 4);
    return s_read;
}

int wosgl_CopyFramebufferGX(int x, int y, int w, int h,
                            int dstW, int dstH, int scale, void *dst)
{ wosgl_gl_touched();
    GLubyte *rgba = read_fb(x, y, w, h);
    unsigned char *out = (unsigned char *)dst;
    int bw = (dstW + 3) >> 2;
    int tx, ty;

    if (!rgba || !dst)
        return 0;
    for (ty = 0; ty < dstH; ty++) {
        for (tx = 0; tx < dstW; tx++) {
            unsigned r = 0, g = 0, b = 0;
            unsigned short gx;
            int sx = tx * scale, sy = ty * scale;
            unsigned char *p = out + (((ty >> 2) * bw + (tx >> 2)) << 5) +
                               ((((ty & 3) << 2) + (tx & 3)) << 1);
            if (sx < w && sy < h) {
                if (scale == 2 && sx + 1 < w && sy + 1 < h) {
                    const GLubyte *a = rgba + ((size_t)sy * w + sx) * 4;
                    const GLubyte *c = a + (size_t)w * 4;
                    r = (a[0] + a[4] + c[0] + c[4] + 2) >> 2;
                    g = (a[1] + a[5] + c[1] + c[5] + 2) >> 2;
                    b = (a[2] + a[6] + c[2] + c[6] + 2) >> 2;
                } else {
                    const GLubyte *a = rgba + ((size_t)sy * w + sx) * 4;
                    r = a[0]; g = a[1]; b = a[2];
                }
            }
            /* GX RGB5A3, opaque: 1 R5 G5 B5 */
            gx = (unsigned short)(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
            p[0] = (unsigned char)(gx >> 8);
            p[1] = (unsigned char)gx;
        }
    }
    return 1;
}

static int capture_into_bound(int srcW, int srcH, int intensity)
{ wosgl_gl_touched();
    WosTex *t = &s_tex[s_cur];
    GLubyte *rgba;
    int w, h, i;

    if (t->cell)
        return 0;               /* not into a cache page */
    if (!s_cur || !t->main || t->w <= 0 || t->h <= 0 || srcW <= 0 || srcH <= 0 || t->fmt != FMT_BGRA)
        return 0;
    drop_one(t);
    w = t->w < srcW ? t->w : srcW;
    h = t->h < srcH ? t->h : srcH;
    rgba = read_fb(0, 0, w, h);
    if (!rgba || !ensure_conv(w * h))
        return 0;
    for (i = 0; i < w * h; i++) {
        const GLubyte *s = rgba + i * 4;
        GLubyte *d = s_conv_main + i * 4;
        if (intensity) {
            GLubyte y = (GLubyte)((s[0] * 77 + s[1] * 150 + s[2] * 29) >> 8);
            d[0] = d[1] = d[2] = d[3] = y;
        } else {
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
        }
    }
    glBindTexture(GL_TEXTURE_2D, t->main);
    WOS_UPB(6, (w) * (h)); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_main);
    /* An EFB copy has no STP bit: both planes show the same texels. */
    if (!t->semi)
        t->semi = make_plane(t->w, t->h, t->wraps, t->wrapt, NULL);
    glBindTexture(GL_TEXTURE_2D, t->semi);
    WOS_UPB(6, (w) * (h)); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_main);
    return 1;
}

int wosgl_CaptureFramebufferTexture(GLsizei srcWidth, GLsizei srcHeight)
{
    return capture_into_bound(srcWidth, srcHeight, 0);
}

int wosgl_CaptureFramebufferIntensityTexture(GLsizei srcWidth, GLsizei srcHeight)
{
    return capture_into_bound(srcWidth, srcHeight, 1);
}

void wosgl_CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                             GLint x, GLint y, GLsizei width, GLsizei height)
{ wosgl_gl_touched();
    WosTex *t = &s_tex[s_cur];
    GLubyte *rgba;
    int i, fbh = wos_video_height();
    (void)target; (void)level;

    if (t->cell)
        return;                 /* not into a cache page */
    if (!s_cur || !t->main || width <= 0 || height <= 0 || !ensure_conv(width * height) || t->fmt != FMT_BGRA)
        return;
    drop_one(t);
    /* GL's (x, y) is the bottom-left corner of the source. */
    rgba = read_fb(x, fbh - y - height, width, height);
    if (!rgba)
        return;
    for (i = 0; i < width * height; i++) {
        /* the texture's row 0 is the lowest framebuffer row, as in GL */
        int row = i / width, col = i % width;
        const GLubyte *s = rgba + ((size_t)(height - 1 - row) * width + col) * 4;
        GLubyte *d = s_conv_main + i * 4;
        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
    }
    glBindTexture(GL_TEXTURE_2D, t->main);
    WOS_UPB(6, (width) * (height)); glTexSubImage2D(GL_TEXTURE_2D, 0, xoffset, yoffset, width, height,
                    MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_main);
}

/* ---- State ------------------------------------------------------------- */

void wosgl_Enable(GLenum cap)
{
    switch (cap) {
    case GL_TEXTURE_2D:   s_texture_enabled = 1; break;
    case GL_BLEND:        s_blend_enabled = 1; break;
    case GL_SCISSOR_TEST: wosgl_batch_end(); s_scissor_on = 1; glEnable(GL_SCISSOR_TEST); break;
    default: break;      /* opengx ignores the rest: alpha test and depth are per draw */
    }
}

void wosgl_Disable(GLenum cap)
{
    switch (cap) {
    case GL_TEXTURE_2D:   s_texture_enabled = 0; break;
    case GL_BLEND:        s_blend_enabled = 0; break;
    case GL_SCISSOR_TEST: wosgl_batch_end(); s_scissor_on = 0; glDisable(GL_SCISSOR_TEST); break;
    default: break;
    }
}

void wosgl_Scissor(GLint x, GLint y, GLsizei width, GLsizei height)
{ wosgl_batch_end();
    s_scissor[0] = x; s_scissor[1] = y; s_scissor[2] = width; s_scissor[3] = height;
    glScissor(x, y, width, height);
}

/* The whole framebuffer to black, scissor or not (the EFB copy's clear). */
void wosgl_ClearFramebuffer(void)
{ wosgl_gl_touched();
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (s_scissor_on)
        glEnable(GL_SCISSOR_TEST);
}

/* opengx's texture cache: nothing to reset over MiniGL. */
void resetTexCacheInfo(void) {}
void wosgl_Viewport(GLint x, GLint y, GLsizei width, GLsizei height)
{ wosgl_batch_end();
    s_vp[0] = x; s_vp[1] = y; s_vp[2] = width; s_vp[3] = height;
    glViewport(x, y, width, height);
}

void wosgl_ClearColor2(unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha)
{
    s_clear[0] = red / 255.0f;
    s_clear[1] = green / 255.0f;
    s_clear[2] = blue / 255.0f;
    s_clear[3] = alpha / 255.0f;
}

void wosgl_Clear(GLbitfield mask)
{ wosgl_gl_touched();
    glClearColor(s_clear[0], s_clear[1], s_clear[2], s_clear[3]);
    glClear(mask);
}

void wosgl_AlphaFunc(GLenum func, GLclampf ref)    { (void)func; (void)ref; }
void wosgl_BlendFunc(GLenum s, GLenum d)           { (void)s; (void)d; }
void wosgl_BlendEquationEXT(GLenum mode)           { (void)mode; }
void wosgl_ColorTableEXT(GLenum target, GLenum internalFormat, GLsizei width,
                         GLenum format, GLenum type, const GLvoid *data)
{
    (void)target; (void)internalFormat; (void)width; (void)format; (void)type; (void)data;
}
void wosgl_ShadeModel(GLenum mode)                 { (void)mode; }
/* 1 once GlesGpu has checked the mask bit (GL_LESS): from then on every
 * primitive writes its depth, the mask bits the checks test (Silent Hill
 * fogs its foliage so: a black textured pass clears the mask, a fog pass
 * checking the mask follows). Games that never check keep the test off. */
static int s_mask_seen;
static void wosgl_state_lost(void);
void wosgl_DepthFunc(GLenum func)
{
    s_zfunc = func;
    if (func != GL_ALWAYS && !s_mask_seen) {
        s_mask_seen = 1;
        wosgl_state_lost();
        wos_log("[gl] mask bit checked: depth written by every primitive from now");
    }
}

static GLenum s_matrix_mode = GL_MODELVIEW;
void wosgl_MatrixMode(GLenum mode)
{ wosgl_batch_end();
    s_matrix_mode = mode;
    if (mode != GL_TEXTURE)
        glMatrixMode(mode);
}
void wosgl_LoadIdentity(void)
{ wosgl_batch_end();
    if (s_matrix_mode == GL_PROJECTION) s_proj_state = 1;
    else if (s_matrix_mode == GL_MODELVIEW) s_mv_identity = 1;
    if (s_matrix_mode != GL_TEXTURE)
        glLoadIdentity();
}
void wosgl_Ortho(int left, int right, int bottom, int top, int near_val, int far_val)
{ wosgl_batch_end();
    if (s_matrix_mode == GL_PROJECTION && s_proj_state == 1 && right != left && top != bottom &&
        far_val != near_val) {
        s_ortho[0] = left; s_ortho[1] = right; s_ortho[2] = bottom;
        s_ortho[3] = top; s_ortho[4] = near_val; s_ortho[5] = far_val;
        s_proj_state = 2;
    } else if (s_matrix_mode == GL_PROJECTION) {
        s_proj_state = 0;
    } else if (s_matrix_mode == GL_MODELVIEW) {
        s_mv_identity = 0;
    }
    glOrtho((GLdouble)left, (GLdouble)right, (GLdouble)bottom, (GLdouble)top,
            (GLdouble)near_val, (GLdouble)far_val);
}
void wosgl_Scalef(GLfloat x, GLfloat y, GLfloat z)
{ wosgl_batch_end();
    if (s_matrix_mode == GL_PROJECTION) s_proj_state = 0;
    else if (s_matrix_mode == GL_MODELVIEW) s_mv_identity = 0;
    if (s_matrix_mode != GL_TEXTURE)
        glScalef(x, y, z);
}
GLenum wosgl_GetError(void)                        { return GL_NO_ERROR; }
const GLubyte *wosgl_GetString(GLenum name)        { wosgl_gl_touched(); return glGetString(name); }
void wosgl_Flush(void)                             { wosgl_gl_touched(); glFlush(); }
void wosgl_Finish(void)                            { wosgl_gl_touched(); glFinish(); }

void wosgl_NoNeedMulConstColor(short flag)         { s_no_need_mul = flag; }
void wosgl_Color4Lcol(unsigned int lcol)
{
    s_lcol[0] = (GLubyte)(lcol >> 24);
    s_lcol[1] = (GLubyte)(lcol >> 16);
    s_lcol[2] = (GLubyte)(lcol >> 8);
    s_lcol[3] = (GLubyte)lcol;
}
void wosgl_SetRGB24(short rgb24)                   { s_rgb24 = rgb24; }
void wosgl_SetTextureMask(short mask)              { (void)mask; }
void wosgl_SetGlobalTextABR(short abr)             { s_abr = abr; }
void wosgl_SetTextureType(short textureSemiType, short loadTextureType, short textureChgType)
{
    (void)loadTextureType; (void)textureChgType;
    s_tex_semi_type = textureSemiType;
}
void wosgl_SetLoadMtxFlg(void)                     {}
void wosgl_SetVramClearedFlg(void)                 {}

/* ---- Primitives -------------------------------------------------------- */

typedef struct {
    float x, y, z, s, t;
    GLubyte c[4];           /* the colour word: R G B A */
} WosVtx;

/* What one pass draws with: the texture plane (0: none), and the vertex
 * colour factor and alpha the TEV formula needs. */
typedef struct {
    GLuint tex;
    float  factor;          /* x the vertex colour */
    float  fixed;           /* or this constant (when >= 0), e.g. 0.5 x white */
    float  alpha;
    int    col5to8;         /* flat untextured colours: fill the low bits */
    int    scale2;          /* the stage scales by 2 */
} WosPass;

/* The GL state a pass draws with: primitives with the same key go into one
 * glBegin/glEnd (MiniGL's cost is mostly per glEnd, some 17 us). */
typedef struct {
    GLuint tex;             /* 0: untextured */
    int    scale2;
    int    blend;           /* 0: alpha; 1-4: the PSX mode */
    GLenum zfunc;
} WosKey;

static struct {
    int    open;            /* a glBegin(GL_TRIANGLES) is open */
    int    n;               /* vertices in it */
    WosKey key;
} s_bt;
static int s_batching = -1;  /* SetEnv WSX_GLBATCH 0: a glBegin per primitive */
#define BATCH_MAX 3000      /* MiniGL takes 8192 vertices */

/* The GL state the draws last set, to leave out calls that change nothing
 * (MiniGL serializes every one; Tekken 3 made some 5300 a frame). Anything
 * else that touches GL forgets it (wosgl_gl_touched). */
static struct {
    int    valid;
    int    blend;           /* the WosKey blend; -1 unknown */
    GLenum zfunc;
    GLuint tex;             /* 0: texturing off */
    int    scale2;
} s_gs;
static void wosgl_state_lost(void) { s_gs.valid = 0; }

static void batch_close(void);
static void fv_init(void);

/* Everything but a draw that touches GL ends the open batch first, and
 * the state the draws set is no longer known. */
/* why batches end: [0] another GL call, [1] texture, [2] blend, [3] depth
 * function, [4] texture scale, [5] full */
unsigned int wos_batch_why[6];
unsigned int wos_tex_seen[64], wos_tex_distinct, wos_tex_onoff, wos_tex_blend, wos_semi_abr[8];   /* textures the draws switch between */

void wosgl_gl_touched(void)
{
    if (s_bt.open)
        wos_batch_why[0]++;
    batch_close();
    s_gs.valid = 0;
}

void wosgl_batch_end(void)
{
    batch_close();
}

/* Screen-space draws of one state, collected as a triangle list and handed
 * to MiniGL in one glDrawArrays(MGL_FLATTRIANGLES) (GL_MGL_flat_triangles):
 * one call for a run of PSX primitives instead of a glBegin/glEnd and a
 * dozen vertex calls each. 24-byte vertices, the layout MiniGL's array
 * fetch loads directly. SetEnv WSX_GLARRAY 0: a glBegin per primitive. */
#ifndef MGL_FLATTRIANGLES
#define MGL_FLATTRIANGLES 0x701E
#endif
typedef struct {
    float   x, y, z, s, t;
    float   r;              /* the palette row of a palette-lookup draw (MiniGL reads it as unit 1's s) */
    GLubyte c[4];
} WosFlatV;
#define FLAT_MAX 8190           /* MiniGL takes 8192 vertices a draw */
static WosFlatV s_fv[FLAT_MAX];
static int s_fn;                /* vertices waiting */
unsigned int wos_vtx_sent, wos_quads_sent;   /* transformed-path vertices, quads among them (for the log) */
static int s_flat_tris = -1;    /* 1: MiniGL has MGL_FLATTRIANGLES */
/* What the waiting vertices are: MGL_FLATTRIANGLES (screen space, draw_flat)
 * or GL_TRIANGLES (MiniGL's transform and its streamed vertex segment,
 * draw); a change of kind closes the batch. */
static GLenum s_fv_mode = MGL_FLATTRIANGLES;

/* A second lane for the transformed arrays (WSX_LANES, default 1): when
 * the state flips between two keys, the primitives of the second key wait
 * in their own run (s_fv from LANE2), drawn after the first's. A primitive
 * of the first key may still join it when it overlaps none of the second
 * lane's (those came before it): then drawing all of the first lane, then
 * all of the second, puts every pixel's primitives in their PSX order.
 * Silent Hill draws its ground as ~250 pairs a frame of an opaque
 * untextured quad under the same quad textured and semi-transparent, each
 * pair a state flip; its tiles meet only at their edges. */
#define LANE2 (FLAT_MAX / 2)            /* 4095 vertices each while two run */
#define LANE_TRIS 512
static int s_lanes = -1;
static int s_fn2;                       /* vertices waiting in lane 2 */
static WosKey s_key2;
static int s_nlt;                       /* lane 2's triangles (for the overlap test) */
static float s_lt[LANE_TRIS][10];       /* x0 y0 x1 y1 x2 y2, bounding box */
unsigned int wos_lane_joins, wos_lane_flush[3];
static void apply_key(const WosKey *k);

static void flat_close(void)
{
    if (s_fn) {
        uint32_t t0 = wos_tb_lo();
        WOS_CRUMB(76);
        glDrawArrays(s_fv_mode, 0, s_fn);
        WOS_CRUMB(77);
        s_fn = 0;
        wos_prof_ticks[WOS_PROF_GLEND] += wos_tb_lo() - t0;
    }
    if (s_fn2) {
        uint32_t t0 = wos_tb_lo();
        apply_key(&s_key2);
        s_bt.key = s_key2;
        memcpy(s_fv, s_fv + LANE2, (size_t)s_fn2 * sizeof(WosFlatV));
        glDrawArrays(s_fv_mode, 0, s_fn2);
        s_fn2 = 0;
        wos_prof_ticks[WOS_PROF_GLEND] += wos_tb_lo() - t0;
    }
    s_nlt = 0;
}

static void tri_box(float *t)
{
    float x0 = t[0], x1 = t[0], y0 = t[1], y1 = t[1];
    int i;
    for (i = 1; i < 3; i++) {
        if (t[2 * i] < x0) x0 = t[2 * i];
        if (t[2 * i] > x1) x1 = t[2 * i];
        if (t[2 * i + 1] < y0) y0 = t[2 * i + 1];
        if (t[2 * i + 1] > y1) y1 = t[2 * i + 1];
    }
    t[6] = x0; t[7] = y0; t[8] = x1; t[9] = y1;
}

/* 1 when the two triangles' insides meet (touching edges do not): a
 * separating axis among the six edge normals, with <= so a shared edge
 * separates */
static int tris_overlap(const float *a, const float *b)
{
    const float *t[2] = { a, b };
    int k, e, i;

    if (a[8] <= b[6] || b[8] <= a[6] || a[9] <= b[7] || b[9] <= a[7])
        return 0;
    for (k = 0; k < 2; k++)
        for (e = 0; e < 3; e++) {
            const float *p = t[k];
            float nx = -(p[2 * ((e + 1) % 3) + 1] - p[2 * e + 1]);
            float ny = p[2 * ((e + 1) % 3)] - p[2 * e];
            float amin = 1e30f, amax = -1e30f, bmin = 1e30f, bmax = -1e30f;
            for (i = 0; i < 3; i++) {
                float da = a[2 * i] * nx + a[2 * i + 1] * ny;
                float db = b[2 * i] * nx + b[2 * i + 1] * ny;
                if (da < amin) amin = da;
                if (da > amax) amax = da;
                if (db < bmin) bmin = db;
                if (db > bmax) bmax = db;
            }
            if (amax <= bmin || bmax <= amin)
                return 0;
        }
    return 1;
}

/* the primitive's triangles (nt vertices of v in order/tri) into tris */
static int prim_tris(const WosVtx *v, const int *order, const int *tri, int nt, float tris[2][10])
{
    int n = nt / 3, i, j;
    for (i = 0; i < n; i++) {
        for (j = 0; j < 3; j++) {
            const WosVtx *p = &v[order[tri[3 * i + j]]];
            tris[i][2 * j] = p->x;
            tris[i][2 * j + 1] = p->y;
        }
        tri_box(tris[i]);
    }
    return n;
}

static int lane2_overlaps(float tris[2][10], int n)
{
    int i, j;
    for (j = 0; j < s_nlt; j++)
        for (i = 0; i < n; i++)
            if (tris_overlap(tris[i], s_lt[j]))
                return 1;
    return 0;
}

static void bt_close(void)
{
    if (s_bt.open) {
        uint32_t t0 = wos_tb_lo();
        s_bt.open = 0;
        glEnd();
        wos_prof_ticks[WOS_PROF_GLEND] += wos_tb_lo() - t0;
    }
}

static void batch_close(void)
{
    bt_close();
    flat_close();
}

static void fv_init(void)
{
    if (s_flat_tris < 0) {
        s_flat_tris = has_ext("GL_MGL_flat_triangles") && wos_env_int("WSX_GLARRAY", 1);
        if (s_flat_tris) {
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_COLOR_ARRAY);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glVertexPointer(3, GL_FLOAT, sizeof(WosFlatV), &s_fv[0].x);
            glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(WosFlatV), s_fv[0].c);
            glTexCoordPointer(3, GL_FLOAT, sizeof(WosFlatV), &s_fv[0].s);
        }
        wos_log("[gl] screen-space draws as %s", s_flat_tris ? "triangle lists (MGL_FLATTRIANGLES)" : "a fan each");
    }
}

static void set_blend_abr(int abr)
{
    glEnable(GL_BLEND);
    switch (abr) {
    case 1:  glBlendFunc(GL_ONE, GL_SRC_ALPHA); break;          /* 0.5B + 0.5F */
    case 3:
        if (s_has_rsub) {
            glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);           /* B - F */
            glBlendFunc(GL_ONE, GL_ONE);
            return;
        }
        glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);            /* B x (1 - F) */
        break;
    default: glBlendFunc(GL_ONE, GL_ONE); break;                /* B + F, B + 0.25F */
    }
    if (s_has_rsub)
        glBlendEquation(GL_FUNC_ADD);
}

static void set_blend_alpha(void)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (s_has_rsub)
        glBlendEquation(GL_FUNC_ADD);
}

/* _ogx_apply_state: 0 when this pass draws nothing. pass 0 is the first (and,
 * blending off or untextured, the only) pass; pass 1 the PSX-blend pass. */
static int s_gp_dbg;          /* WSX_GPDEBUG */
static int s_one_pass = -1;  /* SetEnv WSX_ONEPASS 0: ABR 1 draws twice as before */
static int compute_state(int texen, int color_enabled, int pass, WosPass *ps, WosKey *k)
{
    WosTex *t = &s_tex[s_cur];
    int abr_pass;

    if (s_one_pass < 0)
        s_one_pass = wos_env_int("WSX_ONEPASS", 1);
    ps->tex = 0;
    ps->factor = 1.0f;
    ps->fixed = -1.0f;
    ps->alpha = 1.0f;
    ps->col5to8 = 0;
    ps->scale2 = 0;

    if (texen) {
        /* ABR 1 from the one plane: alpha blending gives F where STP is
         * clear and F/2 + B/2 where it is set, in a single draw. With the
         * texture alpha x 2 (GL_MGL_texture_env_alpha_scale) the vertex
         * alpha picks: 1 makes every texel opaque (2 x 0.5 clamps to 1),
         * 0.5 keeps the plane's 0.5 and 1. Opaque primitives then draw from
         * the same plane in the same state, and the two no longer split
         * MiniGL's records -- they alternate all through a 3D scene. */
        if (t->one && s_one_pass && s_has_ascale && !s_blend_enabled) {
            ps->tex = t->one;
            k->blend = 0;
            ps->alpha = 1.0f;
            if (!color_enabled && s_no_need_mul) ps->fixed = 1.0f;
            else                                 ps->factor = 2.0f;
            if (ps->fixed < 0.0f && s_has_scale2) {
                ps->scale2 = 1;
                ps->factor *= 0.5f;
            }
            if (t->pal && s_gp_dbg) {
                ps->fixed = 1.0f;
                ps->scale2 = 0;
            }
            k->zfunc = s_zfunc;
            k->tex = ps->tex;
            k->scale2 = s_has_scale2 ? ps->scale2 : 0;
            return 1;
        }
        if (s_blend_enabled && s_abr == 1 && t->one && s_one_pass &&
            (s_has_ascale ||
             (s_tex_semi_type & (TEX_TYPE_1 | TEX_TYPE_2)) == (TEX_TYPE_1 | TEX_TYPE_2))) {
            if (pass != 0)
                return 0;
            ps->tex = t->one;
            k->blend = 0;
            ps->alpha = s_has_ascale ? 0.5f : 1.0f;
            if (!color_enabled && s_no_need_mul) ps->fixed = 1.0f;
            else                                 ps->factor = 2.0f;
            if (ps->fixed < 0.0f && s_has_scale2) {
                ps->scale2 = 1;
                ps->factor *= 0.5f;
            }
            k->zfunc = s_zfunc;
            k->tex = ps->tex;
            k->scale2 = s_has_scale2 ? ps->scale2 : 0;
            return 1;
        }
        if (t->pal)             /* only the one-plane cases above draw from it */
            return 0;
        if (s_blend_enabled && pass == 0) {
            if (!(s_tex_semi_type & TEX_TYPE_1) || !t->semi)
                return 0;
            if (t->semi == s_atlas.semi && s_npend[1])
                atlas_flush_cell(1, t->cell);
            ps->tex = t->semi;
            k->blend = 0;
            abr_pass = 0;
        } else {
            if (!(s_tex_semi_type & TEX_TYPE_2) || !t->main)
                return 0;
            if (t->main == s_atlas.main && s_npend[0])
                atlas_flush_cell(0, t->cell);
            ps->tex = t->main;
            if (s_blend_enabled) {
                k->blend = s_abr ? s_abr : 2;
                abr_pass = 1;
            } else {
                k->blend = 0;
                abr_pass = 0;
            }
        }

        /* the TEV colour: texture x (factor x colour | fixed) */
        if (!abr_pass) {
            if (!color_enabled && s_no_need_mul) ps->fixed = 1.0f;
            else                                 ps->factor = 2.0f;
        } else switch (s_abr) {
            case 1:
                if (s_no_need_mul) ps->fixed = 0.5f; else ps->factor = 1.0f;
                ps->alpha = 0.5f;
                break;
            case 4:
                if (s_no_need_mul) ps->fixed = 0.25f; else ps->factor = 0.5f;
                break;
            default:    /* 2, 3 */
                if (s_no_need_mul) ps->fixed = 1.0f; else ps->factor = 2.0f;
                break;
        }
        if (ps->fixed < 0.0f && ps->factor > 1.0f && s_has_scale2) {
            ps->scale2 = 1;
            ps->factor *= 0.5f;
        }
        /* The alpha x 2 applies to these planes too (texels 0 or 1). */
        if (s_has_ascale)
            ps->alpha *= 0.5f;
    } else {
        if (pass != 0)
            return 0;
        if (s_blend_enabled) {
            k->blend = s_abr ? s_abr : 2;
            if (s_abr == 1)      { ps->factor = 0.5f;  ps->alpha = 0.5f; }
            else if (s_abr == 4) { ps->factor = 0.25f; }
        } else {
            k->blend = 0;
        }
        ps->col5to8 = !color_enabled;
    }

    k->zfunc = s_zfunc;
    k->tex = ps->tex;
    k->scale2 = ps->tex && s_has_scale2 ? ps->scale2 : 0;
    return 1;
}

/* The GL state for a key: only what differs from the last. */
static void apply_key(const WosKey *k)
{
    if (!s_gs.valid || s_gs.blend != k->blend) {
        if (k->blend)
            set_blend_abr(k->blend);
        else
            set_blend_alpha();
    }
    if (!s_gs.valid || s_gs.zfunc != k->zfunc) {
        /* GL_ALWAYS is GlesGpu's mask check off, and there GlesGpu turns
         * the depth test off (no depth writes either); the mask bits it
         * tests later come only from primitives drawn while checking. Off
         * here too: MiniGL then sends no Z (5 dwords a vertex instead of
         * 6 across the bus) and the GPU no depth traffic.
         * SetEnv WSX_ZALWAYS 1: the test stays on with GL_ALWAYS, as before. */
        static int keep = -1;
        if (keep < 0)
            keep = wos_env_int("WSX_ZALWAYS", 0);
        if (k->zfunc == GL_ALWAYS && !keep && !s_mask_seen) {
            glDisable(GL_DEPTH_TEST);
        } else {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(k->zfunc);
        }
    }
    if (s_gp.state > 0 && (k->tex == s_gp.idx) != s_gp.gl_on) {
        s_gp.gl_on = k->tex == s_gp.idx;
        glActiveTextureARB(GL_TEXTURE1);
        if (s_gp.gl_on) {
            glBindTexture(GL_TEXTURE_2D, s_gp.pal);
            glEnable(GL_TEXTURE_2D);
        } else
            glDisable(GL_TEXTURE_2D);
        glActiveTextureARB(GL_TEXTURE0);
        if (s_gp.gl_on) glEnable(MGL_PALETTE_LOOKUP); else glDisable(MGL_PALETTE_LOOKUP);
    }
    if (k->tex) {
        /* the texture environment is unit 0's own and stays while texturing
         * is off: sent again only after other GL use (s_gs.valid), not at
         * every untextured -> textured switch (Silent Hill: ~500 a present,
         * three MiniGL calls each) */
        if (!s_gs.valid || !s_gs.tex)
            glEnable(GL_TEXTURE_2D);
        if (!s_gs.valid) {
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            if (s_has_ascale)
                glTexEnvi(GL_TEXTURE_ENV, GL_ALPHA_SCALE, 2);
        }
        if (!s_gs.valid || s_gs.tex != k->tex)
            glBindTexture(GL_TEXTURE_2D, k->tex);
        if (s_has_scale2 && (!s_gs.valid || s_gs.scale2 != k->scale2))
            glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, k->scale2 ? 2.0f : 1.0f);
    } else if (!s_gs.valid || s_gs.tex) {
        glDisable(GL_TEXTURE_2D);
    }
    s_gs.valid = 1;
    s_gs.blend = k->blend;
    s_gs.zfunc = k->zfunc;
    s_gs.tex = k->tex;
    if (k->tex)
        s_gs.scale2 = k->scale2;    /* the env's scale, kept while texturing is off */
}

static inline float clamp1(float v) { return v > 1.0f ? 1.0f : v; }

static inline GLubyte col5to8(GLubyte a) { return (GLubyte)(a | ((a >> 5) & 7)); }

static void emit_color(const WosPass *ps, const GLubyte *c)
{
    float r, g, b;
    if (ps->fixed >= 0.0f) {
        r = g = b = ps->fixed;
    } else if (ps->col5to8) {
        r = clamp1(col5to8(c[0]) / 255.0f * ps->factor);
        g = clamp1(col5to8(c[1]) / 255.0f * ps->factor);
        b = clamp1(col5to8(c[2]) / 255.0f * ps->factor);
    } else {
        r = clamp1(c[0] / 255.0f * ps->factor);
        g = clamp1(c[1] / 255.0f * ps->factor);
        b = clamp1(c[2] / 255.0f * ps->factor);
    }
    glColor4f(r, g, b, ps->alpha);
}

/* MiniGL's ColorByte: what glColor4f would have stored. */
static inline GLubyte color_byte(float v)
{
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return (GLubyte)(v * 255.0f + 0.5f);
}

/* The bytes for one pass's factor, made once per factor and kept: three
 * divisions and four float conversions a vertex were most of the vertex
 * loop. Same bytes as the arithmetic. */
static GLubyte s_ctab[256], s_ctab_alpha;
static float s_ctab_factor = -1.0f, s_ctab_a = -1.0f;
static int s_ctab_5to8 = -1;

static void color_bytes(const WosPass *ps, const GLubyte *c, GLubyte *out)
{
    if (ps->alpha != s_ctab_a) {
        s_ctab_a = ps->alpha;
        s_ctab_alpha = color_byte(ps->alpha);
    }
    out[3] = s_ctab_alpha;
    if (ps->fixed >= 0.0f) {
        out[0] = out[1] = out[2] = color_byte(ps->fixed);
        return;
    }
    if (ps->factor != s_ctab_factor || ps->col5to8 != s_ctab_5to8) {
        int i;
        s_ctab_factor = ps->factor;
        s_ctab_5to8 = ps->col5to8;
        for (i = 0; i < 256; i++)
            s_ctab[i] = color_byte((ps->col5to8 ? col5to8((GLubyte)i) : i) / 255.0f * ps->factor);
    }
    out[0] = s_ctab[c[0]];
    out[1] = s_ctab[c[1]];
    out[2] = s_ctab[c[2]];
}

/* MiniGL's MGL_FLATFAN takes vertices already on the screen and appends
 * them as screen-space triangles: 6 dwords a vertex and a short record
 * header, where its transformed path writes 10 and a 76-dword header per
 * draw over the bus to VRAM (~61 MB/s). The 2D ortho GlesGpu sets is
 * applied here instead. SetEnv WSX_GLFLAT 0: the transformed path. */
static int s_flat = -1;

static void draw_flat(GLenum mode, const WosVtx *v, const int *order, int n,
                      int texen, int color_enabled, int one_color, int passes,
                      float ss, float ts, float os, float ot)
{
    static const int FAN3[3] = { 0, 1, 2 }, FAN4[4] = { 0, 1, 3, 2 };
    const int *fan = (mode == GL_TRIANGLE_STRIP && n == 4) ? FAN4 : FAN3;
    int nf = (mode == GL_TRIANGLE_STRIP && n == 4) ? 4 : 3;
    float fbh = (float)wos_video_height();
    float xs = (float)s_vp[2] / (s_ortho[1] - s_ortho[0]);
    float ys = (float)s_vp[3] / (s_ortho[2] - s_ortho[3]);
    float xo = (float)s_vp[0] - s_ortho[0] * xs;
    float yo = (fbh - (float)s_vp[1] - (float)s_vp[3]) - s_ortho[3] * ys;
    /* window z = 0.5 + 0.5 * ndc z; ndc z = -(2z - (f + n)) / (f - n) */
    float zs = -1.0f / (s_ortho[5] - s_ortho[4]);
    float zo = 0.5f + 0.5f * (s_ortho[5] + s_ortho[4]) / (s_ortho[5] - s_ortho[4]);
    WosPass ps;
    WosKey key;
    int pass, i;

    bt_close();                 /* a transformed batch may still be open */
    fv_init();
    if (s_fv_mode != MGL_FLATTRIANGLES) {
        flat_close();
        s_fv_mode = MGL_FLATTRIANGLES;
    }
    if (passes == 2)
        wos_semi_abr[s_abr & 7]++;
    for (pass = 0; pass < passes; pass++) {
        uint32_t t0 = wos_tb_lo(), t1;
        if (!compute_state(texen, color_enabled, pass, &ps, &key))
            continue;
        if (!s_gs.valid || memcmp(&key, &s_bt.key, sizeof key)) {
            if (key.tex != s_bt.key.tex && (!key.tex || !s_bt.key.tex))
                wos_tex_onoff++;
            if (key.tex != s_bt.key.tex && key.blend != s_bt.key.blend)
                wos_tex_blend++;
            if (key.tex != s_bt.key.tex && key.tex) {
                /* distinct textures this window: a 64-entry set */
                int j, h = key.tex & 63;
                for (j = 0; j < 64 && wos_tex_seen[(h + j) & 63] && wos_tex_seen[(h + j) & 63] != key.tex; j++)
                    ;
                if (j < 64 && !wos_tex_seen[(h + j) & 63]) {
                    wos_tex_seen[(h + j) & 63] = key.tex;
                    wos_tex_distinct++;
                }
            }
            if (s_gs.valid)
                wos_batch_why[key.tex != s_bt.key.tex ? 1 : key.blend != s_bt.key.blend ? 2 :
                              key.zfunc != s_bt.key.zfunc ? 3 : 4]++;
            else
                wos_batch_why[0]++;
            wos_draws++;
            WOS_CRUMB(78);
            flat_close();       /* the waiting triangles have the old state */
            apply_key(&key);
            s_bt.key = key;
        }
        if (ogx_draw_submitted_cb)
            ogx_draw_submitted_cb();
        t1 = wos_tb_lo();
        wos_prof_ticks[WOS_PROF_STATE] += t1 - t0;
        if (s_flat_tris) {
            static const int TRI[6] = { 0, 1, 2, 0, 2, 3 };
            WosFlatV q[4];
            GLubyte one[4];
            int nt = nf == 4 ? 6 : 3;
            if (s_fn + nt > FLAT_MAX)
                flat_close();
            if (!color_enabled || one_color)
                color_bytes(&ps, color_enabled ? v[order[0]].c : s_lcol, one);
            for (i = 0; i < nf; i++) {
                const WosVtx *p = &v[order[fan[i]]];
                q[i].x = xo + p->x * xs;
                q[i].y = yo + p->y * ys;
                q[i].z = zo + p->z * zs;
                q[i].s = texen ? os + p->s * ss : 0.0f;
                q[i].t = texen ? ot + p->t * ts : 0.0f;
                q[i].r = 0.0f;
                if (color_enabled && !one_color)
                    color_bytes(&ps, p->c, q[i].c);
                else
                    memcpy(q[i].c, one, 4);
            }
            for (i = 0; i < nt; i++)
                s_fv[s_fn++] = q[TRI[i]];
            wos_prof_ticks[WOS_PROF_DRAW] += wos_tb_lo() - t1;
            continue;
        }
        WOS_CRUMB(75);
        glBegin(MGL_FLATFAN);
        if (!color_enabled || one_color)
            emit_color(&ps, color_enabled ? v[order[0]].c : s_lcol);
        for (i = 0; i < nf; i++) {
            const WosVtx *p = &v[order[fan[i]]];
            if (color_enabled && !one_color)
                emit_color(&ps, p->c);
            if (texen)
                glTexCoord2f(os + p->s * ss, ot + p->t * ts);
            glVertex3f(xo + p->x * xs, yo + p->y * ys, zo + p->z * zs);
        }
        t0 = wos_tb_lo();
        wos_prof_ticks[WOS_PROF_DRAW] += t0 - t1;
        WOS_CRUMB(76);
        glEnd();
        WOS_CRUMB(77);
        wos_prof_ticks[WOS_PROF_GLEND] += wos_tb_lo() - t0;
    }
}

/* Draw `n` of the primitive's vertices, in `order`, as `mode`: a strip of
 * 4 goes in as two triangles, so that primitives can share a glBegin. */
static void draw(GLenum mode, const WosVtx *v, const int *order, int n,
                 int texen, int color_enabled, int one_color)
{
    static const int TRI1[3] = { 0, 1, 2 }, TRI2[6] = { 0, 1, 2, 2, 1, 3 };
    WosPass ps;
    WosKey key;
    int pass, i;
    int passes = (texen && s_blend_enabled) ? 2 : 1;
    float ss = 1.0f, ts = 1.0f, os = 0.0f, ot = 0.0f;
    const WosTex *bt = &s_tex[s_cur];
    const int *tri = (mode == GL_TRIANGLE_STRIP && n == 4) ? TRI2 : TRI1;
    int nt = (mode == GL_TRIANGLE_STRIP && n == 4) ? 6 : 3;

    if (s_batching < 0)
        s_batching = wos_env_int("WSX_GLBATCH", 1);
    if (texen && bt->cell) {
        if (bt->pal) {
            /* a quarter texel into the texel: GlesGpu's coordinates (u /
             * 255.99) sit on texel edges, and a primitive drawn from one
             * texel (Tekken 3's sky shapes, all four UVs 209,247) took the
             * neighbour; the texture cache's copies have a border of the
             * same texels, the VRAM page here does not */
            ss = 256.0f / GP_W;
            ts = 256.0f / GP_H;
            os = ((float)GP_CELL_X(bt->cell) + 0.25f) / GP_W;
            ot = ((float)GP_CELL_Y(bt->cell) + 0.25f) / GP_H;
        } else {
            /* per cell, made once: two integer divisions, two conversions
             * and two float divisions a primitive were most of draw()'s
             * own time */
            static float cst[ATLAS_CELLS + 1][2];
            static int cst_w;
            static float cst_ss;
            if (cst_w != s_atlas_w) {
                int c;
                cst_w = s_atlas_w;
                cst_ss = 256.0f / s_atlas_w;
                for (c = 1; c <= ATLAS_CELLS; c++) {
                    cst[c][0] = (float)CELL_X(c) / s_atlas_w;
                    cst[c][1] = (float)CELL_Y(c) / ATLAS_H;
                }
            }
            ss = cst_ss;
            ts = 256.0f / ATLAS_H;
            if (bt->cell <= ATLAS_CELLS) {
                os = cst[bt->cell][0];
                ot = cst[bt->cell][1];
            } else {
                os = (float)CELL_X(bt->cell) / s_atlas_w;
                ot = (float)CELL_Y(bt->cell) / ATLAS_H;
            }
        }
    } else if (texen && bt->pw > 0 && bt->ph > 0 && (bt->pw != bt->w || bt->ph != bt->h)) {
        ss = (float)bt->w / (float)bt->pw;
        ts = (float)bt->h / (float)bt->ph;
    }

    /* Off by default since MiniGL streams transformed triangle arrays
     * straight into its vertex segment (DrawTrianglesTclFast, 7-dword
     * vertices): Tekken 3 fights 41 -> 44 fps over MGL_FLATTRIANGLES, whose
     * vertices travel inline through the record, the emitter and the CP
     * copy. SetEnv WSX_GLFLAT 1: screen-space draws as before. */
    if (s_flat < 0)
        s_flat = wos_env_int("WSX_GLFLAT", 0);
    if (s_flat && s_proj_state == 2 && s_mv_identity && s_vp[2] > 0 && s_vp[3] > 0) {
        draw_flat(mode, v, order, n, texen, color_enabled, one_color, passes, ss, ts, os, ot);
        return;
    }

    /* The transformed path as client arrays too: one glDrawArrays
     * (GL_TRIANGLES) for a run of one state, whose vertices MiniGL streams
     * into its vertex segment in VRAM, instead of a glBegin/glEnd with three
     * GL calls a vertex. SetEnv WSX_TCLARRAY 0: glBegin/glEnd as before. */
    {
        static int arrays = -1;
        if (arrays < 0) {
            fv_init();
            arrays = s_flat_tris && s_batching && wos_env_int("WSX_TCLARRAY", 1);
        }
        if (arrays) {
            for (pass = 0; pass < passes; pass++) {
                uint32_t t0 = wos_tb_lo(), t1;
                GLubyte one[4];
                int lane = 0;
                if (!compute_state(texen, color_enabled, pass, &ps, &key))
                    continue;
                bt_close();
                /* An untextured primitive inside a textured batch of the
                 * same blend and depth state: s outside the plane samples
                 * its white border, so texel x colour (x 2 with the RGB
                 * scale, alpha x 2 with the alpha scale) gives the colour
                 * back from the halved bytes. Silent Hill alternates the two
                 * ~500 times a present, each a MiniGL draw of ~25 us. */
                if (!texen) {   /* [prof]: why an untextured one did not join */
                    extern unsigned int wos_untex_why[6];
                    wos_untex_why[!s_border_ok ? 0 : !s_gs.valid || s_fv_mode != GL_TRIANGLES ? 1 :
                                  !s_bt.key.tex ? 2 : key.blend != s_bt.key.blend ? 3 :
                                  key.zfunc != s_bt.key.zfunc ? 4 : 5]++;
                }
                if (!texen && s_border_ok && !s_fn2 && s_gs.valid && s_fv_mode == GL_TRIANGLES &&
                    s_bt.key.tex && key.blend == s_bt.key.blend && key.zfunc == s_bt.key.zfunc &&
                    (s_bt.key.tex == s_atlas.main || s_bt.key.tex == s_atlas.semi ||
                     s_bt.key.tex == s_atlas.one)) {
                    int rs = s_bt.key.scale2, as = s_has_ascale;
                    GLubyte one[4];
                    t1 = wos_tb_lo();
                    wos_prof_ticks[WOS_PROF_STATE] += t1 - t0;
                    if (ogx_draw_submitted_cb)
                        ogx_draw_submitted_cb();
                    if (s_fn + nt > FLAT_MAX)
                        flat_close();
                    if (!color_enabled || one_color)
                        color_bytes(&ps, color_enabled ? v[order[0]].c : s_lcol, one);
                    for (i = 0; i < nt; i++) {
                        const WosVtx *p = &v[order[tri[i]]];
                        WosFlatV *q = &s_fv[s_fn++];
                        q->x = p->x;
                        q->y = p->y;
                        q->z = p->z;
                        q->s = -4.0f;
                        q->t = 0.5f;
                        q->r = 0.0f;
                        if (color_enabled && !one_color)
                            color_bytes(&ps, p->c, q->c);
                        else
                            memcpy(q->c, one, 4);
                        if (rs) {
                            q->c[0] = (GLubyte)((q->c[0] + 1) >> 1);
                            q->c[1] = (GLubyte)((q->c[1] + 1) >> 1);
                            q->c[2] = (GLubyte)((q->c[2] + 1) >> 1);
                        }
                        if (as)
                            q->c[3] = (GLubyte)((q->c[3] + 1) >> 1);
                    }
                    wos_untex_merged++;
                    wos_prof_ticks[WOS_PROF_DRAW] += wos_tb_lo() - t1;
                    wos_vtx_sent += nt;
                    continue;
                }
                {
                    int same1 = s_gs.valid && s_fv_mode == GL_TRIANGLES &&
                                !memcmp(&key, &s_bt.key, sizeof key);
                    float tris[2][10];
                    int ntri = 0;
                    if (s_lanes < 0)
                        s_lanes = wos_env_int("WSX_LANES", 1);
                    lane = 0;
                    if (s_fn2 && !memcmp(&key, &s_key2, sizeof key)) {
                        lane = 1;
                    } else if (same1 && s_fn2) {
                        ntri = prim_tris(v, order, tri, nt, tris);
                        if (s_fn + nt > LANE2 || lane2_overlaps(tris, ntri)) {
                            wos_lane_flush[s_fn + nt > LANE2 ? 1 : 0]++;
                            flat_close();       /* both lanes; the state is lane 2's now */
                            same1 = 0;
                        } else
                            wos_lane_joins++;
                    } else if (!same1 && s_lanes && s_gs.valid && s_fv_mode == GL_TRIANGLES &&
                               s_fn && !s_fn2 && s_fn + nt <= LANE2) {
                        s_key2 = key;       /* lane 2 opens; GL keeps lane 1's state */
                        lane = 1;
                    } else if (!same1 && s_fn2)
                        wos_lane_flush[2]++;
                    if (lane == 1) {
                        if (!ntri)
                            ntri = prim_tris(v, order, tri, nt, tris);
                        if (s_fn2 + nt > LANE2 || s_nlt + ntri > LANE_TRIS) {
                            wos_lane_flush[1]++;
                            flat_close();       /* the state is key's now */
                            lane = 0;
                            same1 = 1;
                        } else {
                            memcpy(s_lt[s_nlt], tris[0], sizeof tris[0]);
                            if (ntri > 1)
                                memcpy(s_lt[s_nlt + 1], tris[1], sizeof tris[1]);
                            s_nlt += ntri;
                        }
                    }
                    if (lane == 1 || same1)
                        goto lane_chosen;
                }
                {
                    if (s_gs.valid && s_fv_mode == GL_TRIANGLES && key.tex != s_bt.key.tex) {
                        /* [prof]: which planes a texture batch end goes between
                         * (0 none, 1 main, 2 semi, 3 one, 4 other) */
                        extern unsigned int wos_texpair[5][5];
#define WOS_PL(t) ((t) == 0 ? 0 : (t) == s_atlas.main ? 1 : (t) == s_atlas.semi ? 2 : (t) == s_atlas.one ? 3 : 4)
                        wos_texpair[WOS_PL(s_bt.key.tex)][WOS_PL(key.tex)]++;
                    }
                    if (s_gs.valid && s_fv_mode == GL_TRIANGLES)
                        wos_batch_why[key.tex != s_bt.key.tex ? 1 : key.blend != s_bt.key.blend ? 2 :
                                      key.zfunc != s_bt.key.zfunc ? 3 : 4]++;
                    else
                        wos_batch_why[0]++;
                    wos_draws++;
                    flat_close();
                    s_fv_mode = GL_TRIANGLES;
                    apply_key(&key);
                    s_bt.key = key;
                }
            lane_chosen:
                if (ogx_draw_submitted_cb)
                    ogx_draw_submitted_cb();
                t1 = wos_tb_lo();
                wos_prof_ticks[WOS_PROF_STATE] += t1 - t0;
                if (!s_fn2 && s_fn + nt > FLAT_MAX)
                    flat_close();
                if (!color_enabled || one_color)
                    color_bytes(&ps, color_enabled ? v[order[0]].c : s_lcol, one);
                for (i = 0; i < nt; i++) {
                    const WosVtx *p = &v[order[tri[i]]];
                    WosFlatV *q = lane ? &s_fv[LANE2 + s_fn2++] : &s_fv[s_fn++];
                    q->x = p->x;
                    q->y = p->y;
                    q->z = p->z;
                    q->s = texen ? os + p->s * ss : 0.0f;
                    q->t = texen ? ot + p->t * ts : 0.0f;
                    q->r = (texen && bt->pal) ? s_gp.rowc : 0.0f;
                    if (color_enabled && !one_color)
                        color_bytes(&ps, p->c, q->c);
                    else
                        memcpy(q->c, one, 4);
                }
                wos_prof_ticks[WOS_PROF_DRAW] += wos_tb_lo() - t1;
                wos_vtx_sent += nt;
                if (nt == 6)
                    wos_quads_sent++;
            }
            return;
        }
    }
    for (pass = 0; pass < passes; pass++) {
        uint32_t t0 = wos_tb_lo(), t1;
        if (!compute_state(texen, color_enabled, pass, &ps, &key))
            continue;
        if (!s_bt.open || !s_batching || s_bt.n + nt > BATCH_MAX ||
            memcmp(&key, &s_bt.key, sizeof key)) {
            if (s_bt.open)
                wos_batch_why[s_bt.n + nt > BATCH_MAX ? 5 : key.tex != s_bt.key.tex ? 1 :
                              key.blend != s_bt.key.blend ? 2 : key.zfunc != s_bt.key.zfunc ? 3 : 4]++;
            wosgl_batch_end();
            t0 = wos_tb_lo();
            WOS_CRUMB(78);
            apply_key(&key);
            wos_draws++;
            glBegin(GL_TRIANGLES);
            s_bt.open = 1;
            s_bt.n = 0;
            s_bt.key = key;
        }
        if (ogx_draw_submitted_cb)
            ogx_draw_submitted_cb();
        t1 = wos_tb_lo();
        wos_prof_ticks[WOS_PROF_STATE] += t1 - t0;
        if (!color_enabled || one_color)
            emit_color(&ps, color_enabled ? v[order[0]].c : s_lcol);
        for (i = 0; i < nt; i++) {
            const WosVtx *p = &v[order[tri[i]]];
            if (color_enabled && !one_color)
                emit_color(&ps, p->c);
            if (texen)
                glTexCoord2f(os + p->s * ss, ot + p->t * ts);
            glVertex3f(p->x, p->y, p->z);
        }
        s_bt.n += nt;
        wos_vtx_sent += nt;
        if (nt == 6)
            wos_quads_sent++;
        if (!s_batching)
            wosgl_batch_end();
        wos_prof_ticks[WOS_PROF_DRAW] += wos_tb_lo() - t1;
    }
}

static const int ORD_0123[4] = { 0, 1, 2, 3 };
static const int ORD_0132[4] = { 0, 1, 3, 2 };
static const int ORD_0213[4] = { 0, 2, 1, 3 };

void wosgl_PRIMdrawTexturedQuad(void *vertexAdr, int changePointOrder)
{
    draw(GL_TRIANGLE_STRIP, (const WosVtx *)vertexAdr,
         changePointOrder ? ORD_0132 : ORD_0123, 4, 1, 0, 0);
}

void wosgl_PRIMdrawTexturedTri(void *vertexAdr)
{
    draw(GL_TRIANGLES, (const WosVtx *)vertexAdr, ORD_0123, 3, 1, 0, 0);
}

void wosgl_PRIMdrawTexGouraudTriColor(void *vertexAdr)
{
    draw(GL_TRIANGLES, (const WosVtx *)vertexAdr, ORD_0123, 3, 1, 1, 0);
}

void wosgl_PRIMdrawTexGouraudTriColorQuad(void *vertexAdr)
{
    draw(GL_TRIANGLE_STRIP, (const WosVtx *)vertexAdr, ORD_0123, 4, 1, 1, 0);
}

void wosgl_PRIMdrawTri(void *vertexAdr)
{
    draw(GL_TRIANGLES, (const WosVtx *)vertexAdr, ORD_0123, 3, 0, 0, 0);
}

void wosgl_PRIMdrawTri2(void *vertexAdr)
{
    draw(GL_TRIANGLE_STRIP, (const WosVtx *)vertexAdr, ORD_0213, 4, 0, 0, 0);
}

void wosgl_PRIMdrawGouraudTriColor(void *vertexAdr)
{
    draw(GL_TRIANGLES, (const WosVtx *)vertexAdr, ORD_0123, 3, 0, 1, 0);
}

void wosgl_PRIMdrawGouraudTri2Color(void *vertexAdr)
{
    draw(GL_TRIANGLE_STRIP, (const WosVtx *)vertexAdr, ORD_0123, 4, 0, 1, 0);
}

void wosgl_PRIMdrawFlatLine(void *vertexAdr)
{
    draw(GL_TRIANGLE_STRIP, (const WosVtx *)vertexAdr, ORD_0132, 4, 0, 1, 1);
}

void wosgl_PRIMdrawGouraudLine(void *vertexAdr)
{
    draw(GL_TRIANGLE_STRIP, (const WosVtx *)vertexAdr, ORD_0132, 4, 0, 1, 0);
}

void wosgl_PRIMdrawQuad(void *vertexAdr)
{
    draw(GL_TRIANGLE_STRIP, (const WosVtx *)vertexAdr, ORD_0132, 4, 0, 0, 0);
}

/* ---- On-screen text (SetEnv WSX_SHOWFPS 1) ----------------------------- */

/* 3x5 glyphs, a row a nibble (bit 2 = left): digits, '.', '/', ' ', 'F', 'P', 'S'. */
static const unsigned short osd_glyph(char c)
{
    static const unsigned short digits[10] = {
        075557, 026222, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717
    };
    if (c >= '0' && c <= '9') return digits[c - '0'];
    switch (c) {
    case '.': return 000002;
    case '/': return 011244;
    case 'F': return 074644;
    case 'P': return 075744;
    case 'S': return 074717;
    default:  return 0;
    }
}

static void fv_init(void);
static void batch_close(void);

/* One screen-space quad of the meter into s_fv, as two triangles */
static int osd_quad(int n, float x, float y, float w, float h, const GLubyte c[4])
{
    static const float qx[6] = { 0, 1, 1, 0, 1, 0 }, qy[6] = { 0, 0, 1, 0, 1, 1 };
    int k;
    if (n + 6 > FLAT_MAX)
        return n;
    for (k = 0; k < 6; k++, n++) {
        s_fv[n].x = x + qx[k] * w;
        s_fv[n].y = y + qy[k] * h;
        s_fv[n].z = 0.0f;
        s_fv[n].s = s_fv[n].t = s_fv[n].r = 0.0f;
        memcpy(s_fv[n].c, c, 4);
    }
    return n;
}

/* Draws `text` at the top left of the screen, over everything, in screen
 * space; leaves the GL state as the draws expect it. One MGL_FLATTRIANGLES
 * draw: a glBegin/glEnd per lit glyph cell was ~70 MiniGL records a frame
 * (~6 ms, Tekken 3 fights 53 -> 31 fps with the meter on). */
void wosgl_draw_osd(const char *text)
{
    static const GLubyte box[4] = { 0, 0, 0, 255 }, ink[4] = { 255, 230, 51, 255 };
    const float px = 3.0f, x0 = 6.0f, y0 = 6.0f;
    int i, row, col, n = (int)strlen(text), nv = 0;

    wosgl_gl_touched();
    fv_init();
    batch_close();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    if (s_flat_tris) {
        nv = osd_quad(nv, x0 - px, y0 - px, (n * 4 + 1) * px, 7 * px, box);
        for (i = 0; i < n; i++) {
            unsigned short g = osd_glyph(text[i]);
            for (row = 0; row < 5; row++)
                for (col = 0; col < 3; col++)
                    if (g & (1u << ((4 - row) * 3 + (2 - col))))
                        nv = osd_quad(nv, x0 + (i * 4 + col) * px, y0 + row * px, px, px, ink);
        }
        glDrawArrays(MGL_FLATTRIANGLES, 0, nv);
        s_fn = 0;
        glEnable(GL_DEPTH_TEST);
        if (s_scissor_on)
            glEnable(GL_SCISSOR_TEST);
        return;
    }
    /* a dark box behind the text */
    glBegin(MGL_FLATFAN);
    glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    glVertex3f(x0 - px, y0 - px, 0.0f);
    glVertex3f(x0 + n * 4 * px, y0 - px, 0.0f);
    glVertex3f(x0 + n * 4 * px, y0 + 6 * px, 0.0f);
    glVertex3f(x0 - px, y0 + 6 * px, 0.0f);
    glEnd();
    for (i = 0; i < n; i++) {
        unsigned short g = osd_glyph(text[i]);
        for (row = 0; row < 5; row++)
            for (col = 0; col < 3; col++) {
                float x, y;
                if (!(g & (1u << ((4 - row) * 3 + (2 - col)))))
                    continue;
                x = x0 + (i * 4 + col) * px;
                y = y0 + row * px;
                glBegin(MGL_FLATFAN);
                glColor4f(1.0f, 0.9f, 0.2f, 1.0f);
                glVertex3f(x, y, 0.0f);
                glVertex3f(x + px, y, 0.0f);
                glVertex3f(x + px, y + px, 0.0f);
                glVertex3f(x, y + px, 0.0f);
                glEnd();
            }
    }
    glEnable(GL_DEPTH_TEST);
    if (s_scissor_on)
        glEnable(GL_SCISSOR_TEST);
}

/* ---- Palette textures on the GPU (s_gp) -------------------------------- */

#ifndef MGL_PALETTE_LOOKUP
#define MGL_PALETTE_LOOKUP 0x7021
#endif

/* The frame read back as RGB565 colours, bottom row first: a texel's name
 * (x in the red 5 and green 6 bits) survives that exactly. */
static int gp_name_at(const GLubyte *rb, int w, int h, int x, int y)
{
    const GLubyte *p = rb + ((size_t)(h - 1 - y) * w + x) * 4;
    return (p[0] >> 3) | ((p[1] >> 2) << 5);
}

static void gp_fill_names(GLubyte *pal, int rows)
{
    int x, y;
    for (y = 0; y < 512; y++)
        for (x = 0; x < 512; x++) {
            GLubyte *p = pal + ((size_t)y * 512 + x) * 4;
            int v = rows ? y : x;
            p[0] = 0;
            p[1] = (GLubyte)((v >> 5) << 2);
            p[2] = (GLubyte)((v & 31) << 3);
            p[3] = 255;
        }
}

/* Measure where the R200's dependent read lands: index v on a palette
 * column, vertex alpha 2k on a palette row (tools/paltest: the index has
 * 8 bits, the interpolated alpha ~7). Needs the context and a w x h
 * 16-bit frame; draws into the back buffer and clears it. SetEnv
 * WSX_GPUPAL 0: the texture cache for every texture, as before. */
void wosgl_gp_init(int w, int h)
{
    GLubyte *pal, *rb;
    int i, ok = 1;

    if (s_gp.state)
        return;
    s_gp.state = -1;
    s_gp_dbg = wos_env_int("WSX_GPDEBUG", 0);
    /* the row rides in the array draws' third texture coordinate: not with
     * WSX_GLFLAT 1 (screen-space draws) or WSX_TCLARRAY 0 (glBegin/glEnd) */
    /* Off by default (SetEnv WSX_GPUPAL 1: on, for tests): the lookup is
     * right, but the palette row cannot reach the shader reliably -- in the
     * vertex alpha the R200's first pass sees the previous primitive's in
     * tiles both touch (some primitives drew another palette's colours),
     * and as unit 1's coordinate (MiniGL MGLPPC_PALVAR 0/1) the GPU locked
     * up, with or without Z. */
    if (!wos_env_int("WSX_GPUPAL", 0) || wos_env_int("WSX_GLFLAT", 0) ||
        !wos_env_int("WSX_TCLARRAY", 1) || !has_ext("GL_MGL_palette_lookup") ||
        !has_ext("GL_MGL_texture_env_alpha_scale") || w < 512 || h < 300)
        return;
    pal = (GLubyte *)malloc((size_t)512 * 512 * 4);
    rb = (GLubyte *)malloc((size_t)w * h * 4);
    if (!pal || !rb || !ensure_conv(2048 * 512)) {
        free(pal); free(rb);
        return;
    }
    s_gp.idx = make_plane(GP_W, GP_H, GL_CLAMP, GL_CLAMP, NULL);
    gp_fill_names(pal, 0);
    s_gp.pal = make_plane(512, 512, GL_CLAMP, GL_CLAMP, pal);
    {   /* index row 0: 0..255 */
        GLubyte ramp[256 * 4];
        for (i = 0; i < 256; i++) {
            ramp[i * 4] = 0; ramp[i * 4 + 1] = 0; ramp[i * 4 + 2] = (GLubyte)i; ramp[i * 4 + 3] = 255;
        }
        glBindTexture(GL_TEXTURE_2D, s_gp.idx);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, ramp);
    }
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_SCISSOR_TEST);
    glActiveTextureARB(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_gp.pal);
    glEnable(GL_TEXTURE_2D);
    glActiveTextureARB(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_gp.idx);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(MGL_PALETTE_LOOKUP);

    /* the index -> the column: one pixel per index, drawn as wos_gl draws
     * (client arrays; the palette row is the texture's third coordinate) */
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    {
        static WosFlatV q[6];
        static const float qx[6] = { 0, 256, 256, 0, 256, 0 }, qy[6] = { 0, 0, 4, 0, 4, 4 };
        int j;
        for (j = 0; j < 6; j++) {
            q[j].x = 100.0f + qx[j];
            q[j].y = 100.0f + qy[j];
            q[j].z = 0.0f;
            q[j].s = qx[j] / GP_W;
            q[j].t = 0.5f / GP_H;
            q[j].r = 1.0f / 512.0f;
            q[j].c[0] = q[j].c[1] = q[j].c[2] = q[j].c[3] = 255;
        }
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(WosFlatV), &q[0].x);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(WosFlatV), q[0].c);
        glTexCoordPointer(3, GL_FLOAT, sizeof(WosFlatV), &q[0].s);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glFinish();
    memset(rb, 0, (size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rb);
    for (i = 0; i < 256; i++) {
        s_gp.col[i] = (short)gp_name_at(rb, w, h, 100 + i, 102);
        if (i && s_gp.col[i] <= s_gp.col[i - 1])
            ok = 0;
    }
    s_gp.col[256] = 512;
    /* the rows: exact, from a texture coordinate; slot k owns rows 2k, 2k+1 */
    s_gp.nslots = GP_SLOTS;
    for (i = 0; i < GP_SLOTS; i++) {
        s_gp.rlo[i] = (short)(2 * i);
        s_gp.rhi[i] = (short)(2 * i + 1);
    }

    glDisable(MGL_PALETTE_LOOKUP);
    glActiveTextureARB(GL_TEXTURE1);
    glDisable(GL_TEXTURE_2D);
    glActiveTextureARB(GL_TEXTURE0);
    glDisable(GL_TEXTURE_2D);
    glClear(GL_COLOR_BUFFER_BIT);
    free(pal);
    free(rb);
    wos_log("[gp] palette lookup %s: index 0,1,15,255 -> column %d,%d,%d,%d; %d palette slots",
            ok ? "calibrated" : "FAILED calibration (texture cache only)",
            s_gp.col[0], s_gp.col[1], s_gp.col[15], s_gp.col[255], s_gp.nslots);
    if (ok)
        s_atlas_w = 1024;       /* the cache keeps the 15-bit textures only */
    if (!ok) {
        char line[1400];
        int len = 0;
        for (i = 0; i < 256 && len < (int)sizeof line - 8; i++)
            len += snprintf(line + len, sizeof line - len, "%d ", s_gp.col[i]);
        wos_log("[gp] columns: %s", line);

        /* their VRAM back: MiniGL keeps 3 copies of each partially updated
         * texture, all through the PPC's 64 MiB aperture */
        glDeleteTextures(1, &s_gp.idx);
        glDeleteTextures(1, &s_gp.pal);
        s_gp.idx = s_gp.pal = 0;
        return;
    }
    s_gp.state = 1;
}

int wosgl_gp_on(void)
{
    return s_gp.state > 0 && s_one_pass && s_has_ascale;
}

/* the wos_gl name GlesGpu binds for index cell c (0..GP_CELLS-1) */
GLuint wosgl_gp_name(int c)
{
    if (!s_gp.name[c] || !s_tex[s_gp.name[c]].pal) {   /* ogx_initialize clears s_tex */
        GLuint n = 0;
        WosTex *t;
        wosgl_GenTextures(1, &n);
        if (!n)
            return 0;
        t = &s_tex[n];
        t->pal = 1;
        t->cell = c + 1;
        t->one = s_gp.idx;
        t->w = t->h = 256;
        t->wraps = t->wrapt = GL_CLAMP;
        s_gp.name[c] = n;
    }
    return s_gp.name[c];
}

/* rows y..y+n-1 of index cell c: 256 x n BGRA texels, red = the index */
void wosgl_gp_index(int c, int y, int n, const GLubyte *bgra)
{
    wosgl_gl_touched();
    glBindTexture(GL_TEXTURE_2D, s_gp.idx);
    (void)glGetError();
    glTexSubImage2D(GL_TEXTURE_2D, 0, GP_CELL_X(c + 1), GP_CELL_Y(c + 1) + y, 256, n,
                    MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, bgra);
    if (glGetError() != GL_NO_ERROR)
        s_gp.fails[0]++;
    s_gp.uploads[0]++;
    s_gp.texels[0] += 256u * (unsigned)n;
}

/* Palette slot k: n colours (PSX halfwords) as the one plane has them --
 * 0 transparent; an opaque primitive's colours opaque but black (with or
 * without STP, as GlesGpu's XP8RGBA_0); a semi-transparent one's 128 where
 * STP is set -- spread over the row's 512 columns by the calibration. */
void wosgl_gp_palette(int k, const unsigned short *pal, int n, int semi)
{
    static GLubyte band[512 * 4 * 8];
    GLubyte *row = band;
    int v, x, y, rows;
    for (v = 0; v < n; v++) {
        unsigned c = pal[v];
        GLubyte e[4];
        /* index v owns the columns halfway to its neighbours' (the index
         * coordinate strays by a column at times, like the alpha) */
        int lo = v ? (s_gp.col[v - 1] + s_gp.col[v]) / 2 + 1 : 0;
        int hi = v + 1 < n ? (s_gp.col[v] + s_gp.col[v + 1]) / 2 : 511;
        if (semi ? c == 0 : (c & 0x7fff) == 0)
            memset(e, 0, 4);
        else
            put_bgra(e, c, (semi && (c & 0x8000)) ? 128 : 255);
        for (x = lo; x <= hi; x++)
            memcpy(row + x * 4, e, 4);
    }
    {   /* SetEnv WSX_GPDEBUG 1: the texels name themselves -- R5 G6 the
         * column, B5 the slot -- to read back what the lookup hit */
        static int dbg = -1;
        if (dbg < 0) dbg = wos_env_int("WSX_GPDEBUG", 0);
        if (dbg)
            for (x = 0; x < 512; x++) {
                row[x * 4] = (GLubyte)((k & 31) << 3);
                row[x * 4 + 1] = (GLubyte)((x >> 5) << 2);
                row[x * 4 + 2] = (GLubyte)((x & 31) << 3);
                row[x * 4 + 3] = 255;
            }
    }
    rows = s_gp.rhi[k] - s_gp.rlo[k] + 1;
    if (rows > 8) rows = 8;
    for (y = 1; y < rows; y++)
        memcpy(band + (size_t)y * 512 * 4, band, 512 * 4);
    wosgl_gl_touched();
    glBindTexture(GL_TEXTURE_2D, s_gp.pal);
    (void)glGetError();
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, s_gp.rlo[k], 512, rows, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, band);
    if (glGetError() != GL_NO_ERROR)
        s_gp.fails[1]++;
    s_gp.uploads[1]++;
    s_gp.texels[1] += 512u * (unsigned)rows;
}

/* the next primitive's palette slot */
void wosgl_gp_slot(int k)
{
    s_gp.rowc = (float)(2 * k + 1) / 512.0f;     /* rows 2k, 2k+1: the edge between, either holds it */
}

int wosgl_gp_nslots(void)
{
    return s_gp.nslots;
}

unsigned wosgl_gp_fails(int k)
{
    return s_gp.fails[k];
}

void wosgl_gp_stats(unsigned *u, unsigned *t)
{
    u[0] = s_gp.uploads[0]; u[1] = s_gp.uploads[1];
    t[0] = s_gp.texels[0]; t[1] = s_gp.texels[1];
    s_gp.uploads[0] = s_gp.uploads[1] = s_gp.texels[0] = s_gp.texels[1] = 0;
}
