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
    int    w, h, fmt;
    int    pw, ph;          /* the planes' real size: w, h but for padded 565 */
    GLint  wraps, wrapt;
    WosPlanes spare[TEX_SPARES];  /* [0] the most recent */
} WosTex;

static WosTex s_tex[MAX_TEX];
static int    s_cur;                    /* the bound (opengx) texture */

static int    s_texture_enabled;
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
static int    s_scissor_on;

void (*ogx_draw_submitted_cb)(void);

/* The texel buffers: one texture's worth (1024 x 512 BGRA) per plane. */
static GLubyte *s_conv_main, *s_conv_semi;
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
    wos_log("[gl] MiniGL: 2x texture scale %s, reverse-subtract blend %s",
            s_has_scale2 ? "yes" : "no (colours clamp at 1.0)",
            s_has_rsub ? "yes" : "no (B-F approximated)");
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
    s_conv_main = (GLubyte *)malloc((size_t)px * 4);
    s_conv_semi = (GLubyte *)malloc((size_t)px * 4);
    if (!s_conv_main || !s_conv_semi) {
        free(s_conv_main); free(s_conv_semi);
        s_conv_main = s_conv_semi = NULL;
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

/* A MiniGL texture of w x h, its texels from bgra (or black). */
static GLuint make_plane_fmt(int w, int h, GLint wraps, GLint wrapt, const GLubyte *bgra, int fmt);
static GLuint make_plane(int w, int h, GLint wraps, GLint wrapt, const GLubyte *bgra)
{
    return make_plane_fmt(w, h, wraps, wrapt, bgra, FMT_BGRA);
}

static GLuint make_plane_fmt(int w, int h, GLint wraps, GLint wrapt, const GLubyte *bgra, int fmt)
{
    GLuint name = 0;
    GLint f = filter_for();
    glGenTextures(1, &name);
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
    if (fmt == FMT_565)
        WOS_PROF(WOS_PROF_TEXUP, glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, MGL_UNSIGNED_SHORT_5_6_5, bgra));
    else
        WOS_PROF(WOS_PROF_TEXUP, glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, bgra));
    wos_prof_reads += 0x10000;      /* texture creations, in the high half */
    return name;
}

static void free_planes(WosTex *t)
{
    int i;
    if (t->main) glDeleteTextures(1, &t->main);
    if (t->semi) glDeleteTextures(1, &t->semi);
    t->main = t->semi = 0;
    t->w = t->h = 0;
    for (i = 0; i < TEX_SPARES; i++) {
        if (t->spare[i].main) glDeleteTextures(1, &t->spare[i].main);
        if (t->spare[i].semi) glDeleteTextures(1, &t->spare[i].semi);
    }
    memset(t->spare, 0, sizeof t->spare);
}

static void set_plane_params(GLuint name, WosTex *t)
{
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
{
    WosPlanes cur, found;
    int i, k = -1;

    if (t->main && t->w == w && t->h == h && t->fmt == fmt)
        return 1;
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
        if (last->main) glDeleteTextures(1, &last->main);
        if (last->semi) glDeleteTextures(1, &last->semi);
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
    GLubyte *m = s_conv_main, *s = s_conv_semi;

    for (y = 0; y < h; y++) {
        const GLubyte *p = src + (size_t)y * pitch * 4;
        for (x = 0; x < w; x++, p += 4, m += 4, s += 4) {
            unsigned v = ((unsigned)p[2] << 8) | p[3];
            if (rgb24) {                            /* movies: one plane */
                put_bgra(m, v, (v & 0x8000) ? 255 : 0);
                memset(s, 0, 4);
                type |= TEX_TYPE_2;
            } else if (v == 0) {
                memset(m, 0, 4);
                memset(s, 0, 4);
            } else if (s_blend_enabled && !(v & 0x8000)) {
                put_bgra(s, v, 255);
                memset(m, 0, 4);
                type |= TEX_TYPE_1;
            } else {
                put_bgra(m, v, 255);
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
{
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
{
    WosTex *t = &s_tex[s_cur];
    if (target != GL_TEXTURE_2D || !s_cur)
        return;
    /* opengx keeps only the wrap modes; the filter follows the settings. */
    if (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T) {
        GLint w = wrap_conv(param);
        if (pname == GL_TEXTURE_WRAP_S) t->wraps = w; else t->wrapt = w;
        if (t->main) {
            glBindTexture(GL_TEXTURE_2D, t->main);
            glTexParameteri(GL_TEXTURE_2D, pname, w);
        }
        if (t->semi) {
            glBindTexture(GL_TEXTURE_2D, t->semi);
            glTexParameteri(GL_TEXTURE_2D, pname, w);
        }
    }
}

void wosgl_TexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    wosgl_TexParameteri(target, pname, (GLint)param);
}

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
{
    WosTex *t = &s_tex[s_cur];
    int type;

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
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pw, ph, GL_RGB, MGL_UNSIGNED_SHORT_5_6_5, s_conv_semi);
            t->w = w;
            t->h = h;
            t->pw = pw;
            t->ph = ph;
            WOS_PROF(WOS_PROF_TEXCONV, to_565((const GLubyte *)data, w, w, h));
        }
        glBindTexture(GL_TEXTURE_2D, t->main);
        WOS_PROF(WOS_PROF_TEXUP, glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, MGL_UNSIGNED_SHORT_5_6_5, s_conv_main));
        return TEX_TYPE_2;
    }
    WOS_PROF(WOS_PROF_TEXCONV, type = split_texels((const GLubyte *)data, w, w, h, rgb24));

    if (!switch_planes(t, w, h, FMT_BGRA)) {
        t->main = make_plane(w, h, t->wraps, t->wrapt, s_conv_main);
        t->w = t->pw = w;
        t->h = t->ph = h;
    } else {
        glBindTexture(GL_TEXTURE_2D, t->main);
        WOS_PROF(WOS_PROF_TEXUP, glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_main));
    }
    if (type & TEX_TYPE_1) {
        if (!t->semi)
            t->semi = make_plane(w, h, t->wraps, t->wrapt, s_conv_semi);
        else {
            glBindTexture(GL_TEXTURE_2D, t->semi);
            WOS_PROF(WOS_PROF_TEXUP, glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_semi));
        }
    }
    return type;
}

int wosgl_TexImage2D(GLenum target, GLint level, GLint internalFormat,
                     GLsizei width, GLsizei height, GLint border,
                     GLenum format, GLenum type, const GLvoid *data)
{
    (void)level; (void)internalFormat; (void)border; (void)format; (void)type;
    if (target != GL_TEXTURE_2D || !data)
        return 0;
    return define_texture(width, height, data, 0);
}

int wosgl_TexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                        GLsizei width, GLsizei height, GLenum format, GLenum type,
                        const GLvoid *data)
{
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
            WOS_PROF(WOS_PROF_TEXUP, glTexSubImage2D(GL_TEXTURE_2D, 0, xoffset, yoffset, cw, ch,
                            GL_RGB, MGL_UNSIGNED_SHORT_5_6_5, s_conv_main));
            return TEX_TYPE_2;
        }
        WOS_PROF(WOS_PROF_TEXCONV, tt = split_texels((const GLubyte *)data, width, cw, ch, 0));
        glBindTexture(GL_TEXTURE_2D, t->main);
        WOS_PROF(WOS_PROF_TEXUP, glTexSubImage2D(GL_TEXTURE_2D, 0, xoffset, yoffset, cw, ch,
                        MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_main));
        if ((tt & TEX_TYPE_1) && !t->semi)
            t->semi = make_plane(t->w, t->h, t->wraps, t->wrapt, NULL);
        if (t->semi) {
            glBindTexture(GL_TEXTURE_2D, t->semi);
            WOS_PROF(WOS_PROF_TEXUP, glTexSubImage2D(GL_TEXTURE_2D, 0, xoffset, yoffset, cw, ch,
                            MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_semi));
        }
    }
    return tt;
}

void wosgl_InitRGBATextures(GLsizei width, GLsizei height)
{
    WosTex *t = &s_tex[s_cur];
    if (!s_cur || !t->used || width <= 0 || height <= 0)
        return;
    if (switch_planes(t, width, height, FMT_BGRA))
        return;                 /* reused: its texels are replaced before use */
    t->main = make_plane(width, height, t->wraps, t->wrapt, NULL);
    t->w = t->pw = width;
    t->h = t->ph = height;
}

void wosgl_ResetMovieTexPtr(void) {}

int wosgl_InitMovieTextures(GLsizei width, GLsizei height, void *texData)
{
    return define_texture(width, height, texData, s_rgb24);
}

/* ---- The framebuffer --------------------------------------------------- */

static GLubyte *s_read;
static int      s_read_px;

/* Read w x h from the framebuffer at (x, y), top-left origin, as RGBA rows
 * top first. */
static GLubyte *read_fb(int x, int y, int w, int h)
{
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
{
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
{
    WosTex *t = &s_tex[s_cur];
    GLubyte *rgba;
    int w, h, i;

    if (!s_cur || !t->main || t->w <= 0 || t->h <= 0 || srcW <= 0 || srcH <= 0 || t->fmt != FMT_BGRA)
        return 0;
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
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_main);
    /* An EFB copy has no STP bit: both planes show the same texels. */
    if (!t->semi)
        t->semi = make_plane(t->w, t->h, t->wraps, t->wrapt, NULL);
    glBindTexture(GL_TEXTURE_2D, t->semi);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_main);
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
{
    WosTex *t = &s_tex[s_cur];
    GLubyte *rgba;
    int i, fbh = wos_video_height();
    (void)target; (void)level;

    if (!s_cur || !t->main || width <= 0 || height <= 0 || !ensure_conv(width * height) || t->fmt != FMT_BGRA)
        return;
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
    glTexSubImage2D(GL_TEXTURE_2D, 0, xoffset, yoffset, width, height,
                    MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_conv_main);
}

/* ---- State ------------------------------------------------------------- */

void wosgl_Enable(GLenum cap)
{
    switch (cap) {
    case GL_TEXTURE_2D:   s_texture_enabled = 1; break;
    case GL_BLEND:        s_blend_enabled = 1; break;
    case GL_SCISSOR_TEST: s_scissor_on = 1; glEnable(GL_SCISSOR_TEST); break;
    default: break;      /* opengx ignores the rest: alpha test and depth are per draw */
    }
}

void wosgl_Disable(GLenum cap)
{
    switch (cap) {
    case GL_TEXTURE_2D:   s_texture_enabled = 0; break;
    case GL_BLEND:        s_blend_enabled = 0; break;
    case GL_SCISSOR_TEST: s_scissor_on = 0; glDisable(GL_SCISSOR_TEST); break;
    default: break;
    }
}

void wosgl_Scissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
    s_scissor[0] = x; s_scissor[1] = y; s_scissor[2] = width; s_scissor[3] = height;
    glScissor(x, y, width, height);
}

/* The whole framebuffer to black, scissor or not (the EFB copy's clear). */
void wosgl_ClearFramebuffer(void)
{
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (s_scissor_on)
        glEnable(GL_SCISSOR_TEST);
}

/* opengx's texture cache: nothing to reset over MiniGL. */
void resetTexCacheInfo(void) {}
void wosgl_Viewport(GLint x, GLint y, GLsizei width, GLsizei height) { glViewport(x, y, width, height); }

void wosgl_ClearColor2(unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha)
{
    s_clear[0] = red / 255.0f;
    s_clear[1] = green / 255.0f;
    s_clear[2] = blue / 255.0f;
    s_clear[3] = alpha / 255.0f;
}

void wosgl_Clear(GLbitfield mask)
{
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
void wosgl_DepthFunc(GLenum func)                  { s_zfunc = func; }

static GLenum s_matrix_mode = GL_MODELVIEW;
void wosgl_MatrixMode(GLenum mode)
{
    s_matrix_mode = mode;
    if (mode != GL_TEXTURE)
        glMatrixMode(mode);
}
void wosgl_LoadIdentity(void)
{
    if (s_matrix_mode != GL_TEXTURE)
        glLoadIdentity();
}
void wosgl_Ortho(int left, int right, int bottom, int top, int near_val, int far_val)
{
    glOrtho((GLdouble)left, (GLdouble)right, (GLdouble)bottom, (GLdouble)top,
            (GLdouble)near_val, (GLdouble)far_val);
}
void wosgl_Scalef(GLfloat x, GLfloat y, GLfloat z)
{
    if (s_matrix_mode != GL_TEXTURE)
        glScalef(x, y, z);
}
GLenum wosgl_GetError(void)                        { return GL_NO_ERROR; }
const GLubyte *wosgl_GetString(GLenum name)        { return glGetString(name); }
void wosgl_Flush(void)                             { glFlush(); }
void wosgl_Finish(void)                            { glFinish(); }

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

static void set_blend_abr(void)
{
    glEnable(GL_BLEND);
    switch (s_abr) {
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
static int apply_state(int texen, int color_enabled, int pass, WosPass *ps)
{
    WosTex *t = &s_tex[s_cur];
    int abr_pass;

    ps->tex = 0;
    ps->factor = 1.0f;
    ps->fixed = -1.0f;
    ps->alpha = 1.0f;
    ps->col5to8 = 0;
    ps->scale2 = 0;

    if (texen) {
        if (s_blend_enabled && pass == 0) {
            if (!(s_tex_semi_type & TEX_TYPE_1) || !t->semi)
                return 0;
            ps->tex = t->semi;
            set_blend_alpha();
            abr_pass = 0;
        } else {
            if (!(s_tex_semi_type & TEX_TYPE_2) || !t->main)
                return 0;
            ps->tex = t->main;
            if (s_blend_enabled) {
                set_blend_abr();
                abr_pass = 1;
            } else {
                set_blend_alpha();
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
    } else {
        if (pass != 0)
            return 0;
        if (s_blend_enabled) {
            set_blend_abr();
            if (s_abr == 1)      { ps->factor = 0.5f;  ps->alpha = 0.5f; }
            else if (s_abr == 4) { ps->factor = 0.25f; }
        } else {
            set_blend_alpha();
        }
        ps->col5to8 = !color_enabled;
    }

    glDepthFunc(s_zfunc);
    if (ps->tex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, ps->tex);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        if (s_has_scale2)
            glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, ps->scale2 ? 2.0f : 1.0f);
    } else {
        glDisable(GL_TEXTURE_2D);
    }
    if (ogx_draw_submitted_cb)
        ogx_draw_submitted_cb();
    return 1;
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

/* Draw `n` of the primitive's vertices, in `order`, as `mode`. */
static void draw(GLenum mode, const WosVtx *v, const int *order, int n,
                 int texen, int color_enabled, int one_color)
{
    WosPass ps;
    int pass, i;
    int passes = (texen && s_blend_enabled) ? 2 : 1;
    float ss = 1.0f, ts = 1.0f;
    const WosTex *bt = &s_tex[s_cur];

    if (texen && bt->pw > 0 && bt->ph > 0 && (bt->pw != bt->w || bt->ph != bt->h)) {
        ss = (float)bt->w / (float)bt->pw;
        ts = (float)bt->h / (float)bt->ph;
    }

    for (pass = 0; pass < passes; pass++) {
        if (!apply_state(texen, color_enabled, pass, &ps))
            continue;
        glBegin(mode);
        if (!color_enabled || one_color)
            emit_color(&ps, color_enabled ? v[order[0]].c : s_lcol);
        for (i = 0; i < n; i++) {
            const WosVtx *p = &v[order[i]];
            if (color_enabled && !one_color)
                emit_color(&ps, p->c);
            if (texen)
                glTexCoord2f(p->s * ss, p->t * ts);
            glVertex3f(p->x, p->y, p->z);
        }
        glEnd();
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
