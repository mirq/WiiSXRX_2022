/*
 * wos_video.c -- the WarpOS build's display: one MiniGL context (the R200
 * stack: minigl_ppc.dll on the PPC, minigl_ppc_host on the 68k).
 *
 * The hardware GPU (GlesGpu) draws into the context itself and calls
 * wos_video_swap(). The software GPUs hand over their finished frame, which
 * is converted to RGB565 here and sent as ONE full-image glTexSubImage2D of a
 * single-level texture of exactly the frame's size: the MiniGL frontend's
 * direct-fill path, the cheapest upload it has (docs/textures.md, 2.1).
 *
 * MiniGL opens the screen or window itself; we only add RAWKEY to its IDCMP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wos_video.h"
#include "wos_sys.h"
void wosgl_gl_touched(void);
#include "wos_amiga.h"
#include "../psxcommon.h"
#include "wos_idle.h"

#include <proto/minigl.h>

static int   s_open;
static int   s_width = 640, s_height = 480;
static void *s_window;

/* The soft frame's texture and its staging buffer. */
static GLuint    s_tex;
static int       s_tex_w, s_tex_h;
static uint16_t *s_stage;
static int       s_stage_px;
static int       s_bilinear = 1;

/* Pacing and stats. */
static uint64_t s_next_frame_us;
static uint64_t s_stat_start_us;
static uint32_t s_stat_frames, s_stat_presents, s_stat_late;
static uint64_t s_stat_upload_us, s_stat_swap_us, s_stat_wait_us;

int   wos_video_width(void)  { return s_width; }
int   wos_video_height(void) { return s_height; }
void *wos_video_window(void) { return s_window; }


/* SetEnv WSX_GLBENCH 1: time MiniGL's immediate mode at startup, the way
 * wos_gl.c draws (4-vertex strips, colour + texcoord per vertex), to tell
 * the cost of a draw from the cost of a state change between draws. */
#ifndef GL_RGB_SCALE
#define GL_RGB_SCALE 0x8573
#endif
static void gl_bench(void)
{
    static GLubyte pix[64 * 64 * 4];
    GLuint tex[2];
    int c, i, k;
    static const char *names[] = {
        "same state, textured", "texture switch every draw", "blend toggle every draw",
        "same state, untextured", "triangles, same state", "texenv scale toggle every draw",
        "GL_QUADS, same state", "quad as 2 triangles, same state"
    };
    glGenTextures(2, tex);
    for (k = 0; k < 2; k++) {
        memset(pix, k ? 0x80 : 0xff, sizeof pix);
        glBindTexture(GL_TEXTURE_2D, tex[k]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    }
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, s_width, s_height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    for (c = 0; c < 8; c++) {
        uint64_t t0, t1;
        int n = 2000;
        glFinish();
        t0 = wos_now_us();
        for (i = 0; i < n; i++) {
            float x = (float)(i % 40) * 16.0f, y = (float)((i / 40) % 30) * 16.0f;
            int textured = c != 3;
            if (textured) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, tex[c == 1 ? (i & 1) : 0]);
            if (c == 2) {
                if (i & 1) { glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); }
                else glDisable(GL_BLEND);
            }
            if (c == 5)
                glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, (i & 1) ? 2.0f : 1.0f);
            if (c == 6) {
                glBegin(GL_QUADS);
                glColor4f(1, 1, 1, 1); glTexCoord2f(0, 0); glVertex3f(x, y, 0);
                glColor4f(1, 1, 1, 1); glTexCoord2f(1, 0); glVertex3f(x + 15, y, 0);
                glColor4f(1, 1, 1, 1); glTexCoord2f(1, 1); glVertex3f(x + 15, y + 15, 0);
                glColor4f(1, 1, 1, 1); glTexCoord2f(0, 1); glVertex3f(x, y + 15, 0);
                glEnd();
            } else if (c == 7) {
                glBegin(GL_TRIANGLES);
                glColor4f(1, 1, 1, 1); glTexCoord2f(0, 0); glVertex3f(x, y, 0);
                glColor4f(1, 1, 1, 1); glTexCoord2f(1, 0); glVertex3f(x + 15, y, 0);
                glColor4f(1, 1, 1, 1); glTexCoord2f(0, 1); glVertex3f(x, y + 15, 0);
                glColor4f(1, 1, 1, 1); glTexCoord2f(0, 1); glVertex3f(x, y + 15, 0);
                glColor4f(1, 1, 1, 1); glTexCoord2f(1, 0); glVertex3f(x + 15, y, 0);
                glColor4f(1, 1, 1, 1); glTexCoord2f(1, 1); glVertex3f(x + 15, y + 15, 0);
                glEnd();
            } else if (c == 4) {
                glBegin(GL_TRIANGLES);
                glColor4f(1, 1, 1, 1); glTexCoord2f(0, 0); glVertex3f(x, y, 0);
                glColor4f(1, 1, 1, 1); glTexCoord2f(1, 0); glVertex3f(x + 15, y, 0);
                glColor4f(1, 1, 1, 1); glTexCoord2f(0, 1); glVertex3f(x, y + 15, 0);
                glEnd();
            } else {
                glBegin(GL_TRIANGLE_STRIP);
                glColor4f(1, 1, 1, 1); glTexCoord2f(0, 0); glVertex3f(x, y, 0);
                glColor4f(1, 1, 1, 1); glTexCoord2f(1, 0); glVertex3f(x + 15, y, 0);
                glColor4f(1, 1, 1, 1); glTexCoord2f(0, 1); glVertex3f(x, y + 15, 0);
                glColor4f(1, 1, 1, 1); glTexCoord2f(1, 1); glVertex3f(x + 15, y + 15, 0);
                glEnd();
            }
        }
        t1 = wos_now_us();
        glFinish();
        wos_log("[glbench] %s: %lu us a draw to submit, %lu us with the finish",
                names[c], (unsigned long)((t1 - t0) / n),
                (unsigned long)((wos_now_us() - t0) / n));
        glDisable(GL_BLEND);
        glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0f);
    }
    glDeleteTextures(2, tex);
    /* MiniGL correctness: a texture created on the direct path (non-mip
     * filter set first) that then gets mip levels after all. */
    {
        GLuint t;
        GLenum e0, e1, e2, e3;
        int lv, sz;
        while (glGetError() != GL_NO_ERROR)
            ;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        memset(pix, 0xc0, sizeof pix);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
        e0 = glGetError();
        for (lv = 1, sz = 32; sz >= 1; lv++, sz >>= 1)
            glTexImage2D(GL_TEXTURE_2D, lv, GL_RGBA, sz, sz, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
        e1 = glGetError();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0, 0); glVertex3f(0, 0, 0);
        glTexCoord2f(1, 0); glVertex3f(8, 0, 0);
        glTexCoord2f(0, 1); glVertex3f(0, 8, 0);
        glTexCoord2f(1, 1); glVertex3f(8, 8, 0);
        glEnd();
        glFinish();
        e2 = glGetError();
        /* and a partial update, then a level again */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 8, 8, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pix);
        glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
        e3 = glGetError();
        wos_log("[glbench] direct texture then mip levels: errors %x %x %x %x (0 = none)",
                (unsigned)e0, (unsigned)e1, (unsigned)e2, (unsigned)e3);
        glDeleteTextures(1, &t);
    }
    mglSwitchDisplay();
}

/* MiniGL's 68k host (minigl_ppc_host) serves one session and ends with it.
 * MiniGL's library starts one itself when its port is missing (MiniGL
 * branch wiistation, R200PPCHostEnsure): WiiStation only gives MiniGL the
 * settings the WiiStation-R200 script makes, those not already set.
 * A host that is already running (the script's) is used as it is. */
#define WOS_HOST_PORT "MiniGLPPC.R200.Host.1"

static int host_port_up(void)
{
    struct MsgPort *p;
    Forbid();
    p = FindPort((CONST_STRPTR)WOS_HOST_PORT);
    Permit();
    return p != NULL;
}

static void env_default(const char *name, const char *value)
{
    char buf[8];
    if (GetVar((CONST_STRPTR)name, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) < 0)
        SetVar((CONST_STRPTR)name, (CONST_STRPTR)value, -1, GVF_GLOBAL_ONLY);
}

void wos_mgl_settings(void)
{
    /* MiniGL's short TCL vertices (read when the client opens): every
     * vertex WiiStation draws has W = 1 (MGLPPC_TCL6: x,y,z,colour,s,t),
     * and Z only matters with the depth test (MGLPPC_TCL5: x,y,colour,s,t
     * without it). Tekken 3 fights ~49 -> ~51 fps: the vertex stream is
     * written across the Zorro bus. SetEnv MGLPPC_TCL6 0: 7 dwords. */
    env_default("MGLPPC_TCL6", "1");
    env_default("MGLPPC_TCL5", "1");
    if (host_port_up())
        return;
    /* for the host MiniGL is about to start */
    DeleteVar((CONST_STRPTR)"MGLPPC_LEASE", GVF_GLOBAL_ONLY);
    DeleteVar((CONST_STRPTR)"MINIGL_CTILE", GVF_GLOBAL_ONLY);   /* a tiled target reads back scrambled */
    env_default("MGLPPC_NOALTIVEC", "1");
    env_default("MGLPPC_CPEMIT", "1");
    env_default("MGLPPC_HOSTPRI", "-1");
    env_default("MGLPPC_CPPRESENT", "1");
}

int wos_video_open(void)
{
    int windowed = wos_env_int("WSX_WINDOW", 0);

    s_width    = wos_env_int("WSX_WIDTH", 640);
    s_height   = wos_env_int("WSX_HEIGHT", 480);
    s_bilinear = wos_env_int("WSX_BILINEAR", 1);

    wos_mgl_settings();     /* main() already did */
    if (!MiniGLOpen()) {
        wos_log("MiniGLOpen failed: is minigl_ppc.dll in PROGDIR: and minigl_ppc_host running?");
        return -1;
    }
    {   /* where the DLL sits, for the [pcprof] samples */
        extern void *MiniGLBase;
        extern void *dllGetProcAddress(void *hinst, char *name);
        wos_log("[diag] MiniGL MGLGetDispatchTable at %p",
                MiniGLBase ? dllGetProcAddress(MiniGLBase, (char *)"MGLGetDispatchTable") : NULL);
    }
    mglChooseWindowMode(windowed ? GL_TRUE : GL_FALSE);
    mglChoosePixelDepth(16);
    mglChooseNumberOfBuffers(2);
    if (!mglCreateContext(0, 0, s_width, s_height)) {
        wos_log("mglCreateContext %dx%d failed", s_width, s_height);
        MiniGLClose();
        return -1;
    }
    mglEnableSync(wos_env_int("WSX_VSYNC", 0) ? GL_TRUE : GL_FALSE);
    mglLockMode(MGL_LOCK_SMART);

    s_window = mglGetWindowHandle();
    if (s_window)
        ModifyIDCMP((struct Window *)s_window,
                    IDCMP_RAWKEY | IDCMP_CLOSEWINDOW | IDCMP_INACTIVEWINDOW);

    glViewport(0, 0, s_width, s_height);
    {   /* GPU palette lookup: where its dependent read lands (wos_gl.c) */
        extern void wosgl_gp_init(int w, int h);
        wosgl_gp_init(s_width, s_height);
    }
    glViewport(0, 0, s_width, s_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    mglSwitchDisplay();

    if (wos_env_int("WSX_GLBENCH", 0))
        gl_bench();
    s_open = 1;
    s_stat_start_us = wos_now_us();
    wos_log("display %dx%d %s via MiniGL (%s)", s_width, s_height,
            windowed ? "window" : "screen", (const char *)glGetString(GL_RENDERER));
    return 0;
}

void wos_video_close(void)
{
    if (!s_open)
        return;
    if (s_tex)
        glDeleteTextures(1, &s_tex);
    s_tex = 0;
    mglDeleteContext();
    MiniGLClose();
    free(s_stage);
    s_stage = NULL;
    s_stage_px = 0;
    s_window = NULL;
    s_open = 0;
}

/* ---- The software GPUs' frame ---------------------------------------- */

static int ensure_texture(int w, int h)
{
    if (s_stage_px < w * h) {
        free(s_stage);
        s_stage = (uint16_t *)malloc((size_t)w * h * 2);
        s_stage_px = s_stage ? w * h : 0;
        if (!s_stage)
            return 0;
    }
    if (s_tex && s_tex_w == w && s_tex_h == h)
        return 1;
    if (!s_tex)
        glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s_bilinear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s_bilinear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    memset(s_stage, 0, (size_t)w * h * 2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, MGL_UNSIGNED_SHORT_5_6_5, s_stage);
    s_tex_w = w;
    s_tex_h = h;
    wos_log("soft frame texture %dx%d", w, h);
    return 1;
}

/* PSX VRAM halfwords are little-endian BGR555 (R in the low bits); RGB565 out. */
static void convert_15(const uint8_t *src, int pitch, int w, int h, uint16_t *dst)
{
    int x, y;
    for (y = 0; y < h; y++) {
        const uint8_t *s = src + y * pitch;
        for (x = 0; x < w; x++) {
            uint32_t v = s[0] | (s[1] << 8);
            uint32_t r = v & 0x1f, g = (v >> 5) & 0x1f, b = (v >> 10) & 0x1f;
            *dst++ = (uint16_t)((r << 11) | (g << 6) | ((g >> 4) << 5) | b);
            s += 2;
        }
    }
}

/* 24-bit mode (MDEC movies): packed R, G, B bytes. */
static void convert_24(const uint8_t *src, int pitch, int w, int h, uint16_t *dst)
{
    int x, y;
    for (y = 0; y < h; y++) {
        const uint8_t *s = src + y * pitch;
        for (x = 0; x < w; x++) {
            *dst++ = (uint16_t)(((s[0] & 0xf8) << 8) | ((s[1] & 0xfc) << 3) | (s[2] >> 3));
            s += 3;
        }
    }
}

static void draw_textured_quad(void)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 1.0, 1.0, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glBegin(GL_TRIANGLE_FAN);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, 0.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(1.0f, 0.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(1.0f, 1.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f, 1.0f);
    glEnd();
}

/* SetEnv WSX_SHOWFPS 1: the emulation's frames a second (60 is full speed)
 * and the game's own (presents) at the top left, over each half second. */
static uint32_t s_osd_frames, s_osd_presents;
static uint64_t s_osd_t0;
static char s_osd_text[24];
static int s_osd_on = -1;

static void osd_draw(void)
{
    extern void wosgl_draw_osd(const char *text);
    uint64_t now = wos_now_us();
    static uint32_t checks;
    if (s_osd_on < 0 || !(++checks & 1023))
        s_osd_on = wos_env_int("WSX_SHOWFPS", 0);
    if (!s_osd_on)
        return;
    if (!s_osd_t0)
        s_osd_t0 = now;
    if (now - s_osd_t0 >= 500000) {
        unsigned long f10 = (unsigned long)((uint64_t)s_osd_frames * 10000000u / (now - s_osd_t0));
        unsigned long p = (unsigned long)(((uint64_t)s_osd_presents * 1000000u + (now - s_osd_t0) / 2) / (now - s_osd_t0));
        snprintf(s_osd_text, sizeof s_osd_text, "%lu.%lu/%lu", f10 / 10, f10 % 10, p);
        s_osd_t0 = now;
        s_osd_frames = s_osd_presents = 0;
    }
    if (s_osd_text[0])
        wosgl_draw_osd(s_osd_text);
}

void wos_video_present_vram(const void *src, int pitch, int rgb24, int w, int h)
{
    wosgl_gl_touched();      /* the primitives drawn so far first; GL state changes here */
    uint64_t t0, t1;

    if (!s_open || !src || w <= 0 || h <= 0)
        return;
    if (w > 1024) w = 1024;
    if (h > 512)  h = 512;
    if (!ensure_texture(w, h))
        return;

    t0 = wos_now_us();
    if (rgb24)
        convert_24((const uint8_t *)src, pitch, w, h, s_stage);
    else
        convert_15((const uint8_t *)src, pitch, w, h, s_stage);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, MGL_UNSIGNED_SHORT_5_6_5, s_stage);
    t1 = wos_now_us();
    s_stat_upload_us += t1 - t0;

    draw_textured_quad();
    wos_video_swap();
}

void wos_video_present_blank(void)
{
    wosgl_gl_touched();      /* the primitives drawn so far first; GL state changes here */
    if (!s_open)
        return;
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    wos_video_swap();
}

static unsigned s_stat_presents_total;
void wos_video_swap(void)
{
    wosgl_gl_touched();      /* the primitives drawn so far first; GL state changes here */
    uint64_t t0;

    if (!s_open)
        return;
    WOS_CRUMB(21);
    osd_draw();
    s_osd_presents++;
    {   /* SetEnv WSX_SHOT n (or F8): present n read back into T:shot.ppm (to look at
         * the picture: the bridge's screenshot of the MiniGL screen is grey) */
        static int shot = -2;
        if (shot == -2)
            shot = wos_env_int("WSX_SHOT", -1);
        extern int wos_shot_req;
        if ((shot >= 0 && s_stat_presents_total == (unsigned)shot) || wos_shot_req) {
            wos_shot_req = 0;
            unsigned char *rb = malloc((size_t)s_width * s_height * 4);
            BPTR fh;
            if (rb) {
                glReadPixels(0, 0, s_width, s_height, GL_RGBA, GL_UNSIGNED_BYTE, rb);
                fh = Open((CONST_STRPTR)"T:shot.ppm", MODE_NEWFILE);
                if (fh) {
                    char hdr[32];
                    int y, x, n = snprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", s_width, s_height);
                    unsigned char *row = malloc((size_t)s_width * 3);
                    Write(fh, hdr, n);
                    for (y = s_height - 1; row && y >= 0; y--) {
                        const unsigned char *p = rb + (size_t)y * s_width * 4;
                        for (x = 0; x < s_width; x++) {
                            row[x * 3] = p[x * 4]; row[x * 3 + 1] = p[x * 4 + 1]; row[x * 3 + 2] = p[x * 4 + 2];
                        }
                        Write(fh, row, s_width * 3);
                    }
                    free(row);
                    Close(fh);
                    wos_log("[shot] present %d in T:shot.ppm", shot);
                }
                free(rb);
            }
            wosgl_gl_touched();
        }
        s_stat_presents_total++;
        if (wos_pmc_on)
            wos_pcprof_poll(s_stat_presents_total);
    }
    t0 = wos_now_us();
    mglSwitchDisplay();
    s_stat_swap_us += wos_now_us() - t0;
    s_stat_presents++;
}

/* ---- Pacing ------------------------------------------------------------ */

/* busy time a frame (from the end of the last wait to this limit call),
 * by ms: <14, 14, 15, 16, 16.7, 17, 18, 19, 20-24, >=25 */
static uint32_t s_busy_hist[10], s_busy_max;
static uint64_t s_busy_from;

void wos_video_limit(float hz)
{
    uint64_t period, now;

    if (hz < 1.0f)
        return;
    period = (uint64_t)(1000000.0f / hz);
    now = wos_now_us();
    if (s_busy_from) {
        uint32_t b = (uint32_t)(now - s_busy_from);
        int k = b < 14000 ? 0 : b < 15000 ? 1 : b < 16000 ? 2 : b < 16700 ? 3 : b < 17000 ? 4 :
                b < 18000 ? 5 : b < 19000 ? 6 : b < 20000 ? 7 : b < 25000 ? 8 : 9;
        s_busy_hist[k]++;
        if (b > s_busy_max)
            s_busy_max = b;
        {   /* a slow frame: where its time went (time-base ticks -> us) */
            static uint32_t prev[WOS_PROF_N], fl_prev, flus_prev;
            static int spikes = 40;
            extern uint32_t wos_log_flushes, wos_log_flush_us;
            uint32_t per = wos_tb_hz() / 1000000u;
            if (b >= 25000 && spikes > 0 && s_stat_presents_total > 200) {
                char line[400];
                int i, len;
                spikes--;
                len = snprintf(line, sizeof line, "[spike] present %u: %lu us; log flushes %lu (%lu us); stages us:",
                               s_stat_presents_total, (unsigned long)b,
                               (unsigned long)(wos_log_flushes - fl_prev), (unsigned long)(wos_log_flush_us - flus_prev));
                for (i = 0; i < WOS_PROF_N && len < (int)sizeof line - 16; i++) {
                    uint32_t d = (wos_prof_ticks[i] - prev[i]) / per;
                    if (d >= 500)
                        len += snprintf(line + len, sizeof line - len, " %d:%lu", i, (unsigned long)d);
                }
                wos_log("%s", line);
            }
            memcpy(prev, wos_prof_ticks, sizeof prev);
            fl_prev = wos_log_flushes;
            flus_prev = wos_log_flush_us;
        }
    }
    if (!s_next_frame_us || now > s_next_frame_us + 8 * period) {
        /* first frame, or far behind: start again from now */
        s_next_frame_us = now + period;
        s_stat_late++;
        return;
    }
    if (now < s_next_frame_us) {
        WOS_CRUMB2(13, (uint32_t)(s_next_frame_us - now));
        s_stat_wait_us += s_next_frame_us - now;
        wos_wait_until_us(s_next_frame_us);
    } else {
        s_stat_late++;
    }
    s_next_frame_us += period;
    s_busy_from = wos_now_us();
}

void wos_video_frame_stats(void)
{
    uint64_t now, span;

    s_osd_frames++;
    if (++s_stat_frames < 300)
        return;
    now = wos_now_us();
    span = now - s_stat_start_us;
    if (span) {
        unsigned long fps100 = (unsigned long)((uint64_t)s_stat_frames * 100000000u / span);
        uint64_t per = (uint64_t)wos_tb_hz() / 1000000u * s_stat_frames;
        int i;
        unsigned long st[WOS_PROF_N];
        for (i = 0; i < WOS_PROF_N; i++) {
            st[i] = per ? (unsigned long)(wos_prof_ticks[i] / per) : 0;
            wos_prof_ticks[i] = 0;
        }
        wos_pc_report();
        wos_pmc_report(s_stat_frames);
        /* rcnt holds lace and spu; irq holds cd */
        wos_log("[prof] us per frame %lu: gpu %lu, rcnt %lu (lace/present %lu, spu %lu), irq %lu (cd read %lu), mdec %lu, the rest (cpu) %lu",
                (unsigned long)(span / s_stat_frames), st[WOS_PROF_GPU], st[WOS_PROF_RCNT],
                st[WOS_PROF_LACE], st[WOS_PROF_SPU], st[WOS_PROF_IRQ], st[WOS_PROF_CDR],
                st[WOS_PROF_MDEC],
                (unsigned long)(span / s_stat_frames) - st[WOS_PROF_GPU] - st[WOS_PROF_RCNT]
                    - st[WOS_PROF_IRQ] - st[WOS_PROF_MDEC] - st[WOS_PROF_GP1]);
        {
            extern unsigned int wos_tex_subs, wos_tex_texels, wos_draws;
            wos_log("[prof] MiniGL: state %lu us, vertices %lu us, glEnd %lu us, %lu draws a frame; GlesGpu texture lookup %lu us",
                    st[WOS_PROF_STATE], st[WOS_PROF_DRAW], st[WOS_PROF_GLEND], (unsigned long)(wos_draws / s_stat_frames),
                    st[WOS_PROF_TEXSEL]);
            wos_draws = 0;
            {
                extern unsigned int wos_vtx_sent, wos_quads_sent;
                wos_log("[prof] transformed path: %lu vertices a frame, %lu quads (as 2 triangles)",
                        (unsigned long)(wos_vtx_sent / s_stat_frames), (unsigned long)(wos_quads_sent / s_stat_frames));
                wos_vtx_sent = wos_quads_sent = 0;
            }
            {   /* inside the lookup: CLUT key, cache search, load (upload included); calls */
                extern uint32_t wos_ts_ticks[4];
                extern unsigned int wos_ts_calls;
                uint64_t per = (uint64_t)wos_tb_hz() / 1000000u * s_stat_frames;
                { extern unsigned int wos_twin_n; static unsigned int twin_prev;
                  wos_log("[prof] texture windows: %u lookups a frame", (wos_twin_n - twin_prev) / (unsigned)s_stat_frames);
                  twin_prev = wos_twin_n; }
                { extern unsigned int wos_texpair[5][5]; char l[300]; int i, j, n = 0;
                  n += snprintf(l, sizeof l, "[prof] texture batch ends (from>to, 0 none 1 main 2 semi 3 one 4 other) in 300 frames:");
                  for (i = 0; i < 5; i++) for (j = 0; j < 5; j++)
                      if (wos_texpair[i][j] && n < (int)sizeof l - 20) n += snprintf(l + n, sizeof l - n, " %d>%d %u", i, j, wos_texpair[i][j]);
                  wos_log("%s", l); memset(wos_texpair, 0, sizeof wos_texpair);
                  { extern unsigned int wos_untex_merged;
                    extern unsigned int wos_untex_why[6];
                    wos_log("[prof] untextured primitives drawn in a textured batch: %u a frame; "
                            "not/joined (no border, no batch, untextured batch, blend, depth, ok): %u %u %u %u %u %u",
                            wos_untex_merged / (unsigned)s_stat_frames, wos_untex_why[0], wos_untex_why[1],
                            wos_untex_why[2], wos_untex_why[3], wos_untex_why[4], wos_untex_why[5]);
                    memset(wos_untex_why, 0, sizeof wos_untex_why);
                    wos_untex_merged = 0; }
                  { extern unsigned int wos_hw_hist[4096];   /* the busiest hardware reads */
                    char l[200]; int n = 0, k, i;
                    for (k = 0; k < 6; k++) {
                        int best = 0;
                        for (i = 1; i < 4096; i++) if (wos_hw_hist[i] > wos_hw_hist[best]) best = i;
                        if (!wos_hw_hist[best]) break;
                        n += snprintf(l + n, sizeof l - n, " 1f80%04x %u", best * 2 + 0x0000, wos_hw_hist[best] / (unsigned)s_stat_frames);
                        wos_hw_hist[best] = 0;
                    }
                    wos_log("[prof] hw reads a frame:%s", l);
                    memset(wos_hw_hist, 0, sizeof wos_hw_hist); }
                  { extern unsigned int wos_lane_joins, wos_lane_flush[3];
                    wos_log("[prof] second lane: %u joins a frame; flushed by overlap %u, room %u, a third state %u",
                            wos_lane_joins / (unsigned)s_stat_frames, wos_lane_flush[0], wos_lane_flush[1], wos_lane_flush[2]);
                    wos_lane_joins = 0; memset(wos_lane_flush, 0, sizeof wos_lane_flush); } }
                wos_log("[prof] texture lookup inside: CLUT key %lu us, search %lu us, load %lu us; %u lookups a frame",
                        per ? (unsigned long)(wos_ts_ticks[0] / per) : 0, per ? (unsigned long)(wos_ts_ticks[1] / per) : 0,
                        per ? (unsigned long)(wos_ts_ticks[2] / per) : 0, wos_ts_calls / (unsigned)s_stat_frames);
                {
                    extern unsigned int wos_gp_prims, wos_gp_rows, wos_gp_pals, wos_gp_fall;
                    extern void wosgl_gp_stats(unsigned *u, unsigned *t);
                    unsigned u[2], t[2];
                    wosgl_gp_stats(u, t);
                    wos_log("[gp] GPU palette lookup a frame: %u primitives (%u fell back), %u index rows and %u palettes converted; uploads %u index (%u texels), %u palette",
                            wos_gp_prims / (unsigned)s_stat_frames, wos_gp_fall / (unsigned)s_stat_frames,
                            wos_gp_rows / (unsigned)s_stat_frames, wos_gp_pals / (unsigned)s_stat_frames,
                            u[0] / (unsigned)s_stat_frames, t[0] / (unsigned)s_stat_frames, u[1] / (unsigned)s_stat_frames);
                    {
                        extern unsigned wosgl_gp_fails(int k);
                        wos_log("[gp] failed uploads so far: %u index, %u palette", wosgl_gp_fails(0), wosgl_gp_fails(1));
                    }
                    wos_gp_prims = wos_gp_rows = wos_gp_pals = wos_gp_fall = 0;
                }
                {
                    extern unsigned int wos_ts_probe, wos_ts_hhit;
                    wos_log("[prof] texture search hash: %u of %u lookups hit", wos_ts_hhit, wos_ts_probe);
                    wos_ts_probe = wos_ts_hhit = 0;
                }
                wos_ts_ticks[0] = wos_ts_ticks[1] = wos_ts_ticks[2] = 0;
                wos_ts_calls = 0;
            }
            {
                extern unsigned int wos_batch_why[6];
                extern unsigned int wos_tex_seen[64], wos_tex_distinct, wos_tex_onoff, wos_tex_blend, wos_semi_abr[8];
                wos_log("[prof] batches end for: another GL call %u, texture %u (%u of them texturing on/off, %u with the blend too), blend %u, depth %u, scale %u, full %u; %u textures switched between",
                        wos_batch_why[0] / s_stat_frames, wos_batch_why[1] / s_stat_frames, wos_tex_onoff / s_stat_frames, wos_tex_blend / s_stat_frames,
                        wos_batch_why[2] / s_stat_frames,
                        wos_batch_why[3] / s_stat_frames, wos_batch_why[4] / s_stat_frames, wos_batch_why[5] / s_stat_frames,
                        wos_tex_distinct);
                wos_tex_onoff = wos_tex_blend = 0;
                {
                    extern unsigned int wos_gl_tex_made, wos_gl_tex_deleted;
                    wos_log("[gl] MiniGL textures live %u (made %u, deleted %u)",
                            wos_gl_tex_made - wos_gl_tex_deleted, wos_gl_tex_made, wos_gl_tex_deleted);
                }
                wos_log("[prof] two-pass textured primitives a frame by blend mode: 0:%u 1:%u 2:%u 3:%u 4:%u",
                        wos_semi_abr[0] / s_stat_frames, wos_semi_abr[1] / s_stat_frames, wos_semi_abr[2] / s_stat_frames,
                        wos_semi_abr[3] / s_stat_frames, wos_semi_abr[4] / s_stat_frames);
                memset(wos_semi_abr, 0, sizeof wos_semi_abr);
                memset(wos_tex_seen, 0, sizeof wos_tex_seen);
                wos_tex_distinct = 0;
                memset(wos_batch_why, 0, sizeof wos_batch_why);

            }
            { extern unsigned int wos_tex_unsync, wos_defer_rects, wos_defer_flushes;
            wos_log("[prof] gp1 %lu us, framebuffer reads %lu us (%lu reads), textures: convert %lu us, upload %lu us, %lu partial uploads (%lu with a sync point) of %lu texels a frame; main/semi deferred %lu, flushes %lu in %lu frames",
                    st[WOS_PROF_GP1], st[WOS_PROF_READ], (unsigned long)(wos_prof_reads & 0xff),
                    st[WOS_PROF_TEXCONV], st[WOS_PROF_TEXUP],
                    (unsigned long)(wos_tex_subs / s_stat_frames), (unsigned long)(wos_tex_unsync / s_stat_frames),
                    (unsigned long)(wos_tex_texels / s_stat_frames),
                    (unsigned long)wos_defer_rects, (unsigned long)wos_defer_flushes, (unsigned long)s_stat_frames);
            wos_tex_subs = wos_tex_texels = wos_tex_unsync = wos_defer_rects = wos_defer_flushes = 0; }
            { extern unsigned int wos_up_texels[8], wos_up_calls[8];
              int k;
              wos_log("[prof] texels sent a frame (calls in %lu frames): new %lu (%lu), whole %lu (%lu), cache %lu (%lu), deferred %lu (%lu), cell clear %lu (%lu), movie %lu (%lu), fb copy %lu (%lu), other %lu (%lu)",
                      (unsigned long)s_stat_frames,
                      (unsigned long)(wos_up_texels[0] / s_stat_frames), (unsigned long)wos_up_calls[0],
                      (unsigned long)(wos_up_texels[1] / s_stat_frames), (unsigned long)wos_up_calls[1],
                      (unsigned long)(wos_up_texels[2] / s_stat_frames), (unsigned long)wos_up_calls[2],
                      (unsigned long)(wos_up_texels[3] / s_stat_frames), (unsigned long)wos_up_calls[3],
                      (unsigned long)(wos_up_texels[4] / s_stat_frames), (unsigned long)wos_up_calls[4],
                      (unsigned long)(wos_up_texels[5] / s_stat_frames), (unsigned long)wos_up_calls[5],
                      (unsigned long)(wos_up_texels[6] / s_stat_frames), (unsigned long)wos_up_calls[6],
                      (unsigned long)(wos_up_texels[7] / s_stat_frames), (unsigned long)wos_up_calls[7]);
              { extern uint64_t wos_up_ticks[8];
wos_log("[prof] upload us a frame by kind: new %lu, whole %lu, cache %lu, deferred %lu, cell clear %lu, movie %lu, fb copy %lu, other %lu",
                        (unsigned long)(per ? wos_up_ticks[0] / per : 0), (unsigned long)(per ? wos_up_ticks[1] / per : 0),
                        (unsigned long)(per ? wos_up_ticks[2] / per : 0), (unsigned long)(per ? wos_up_ticks[3] / per : 0),
                        (unsigned long)(per ? wos_up_ticks[4] / per : 0), (unsigned long)(per ? wos_up_ticks[5] / per : 0),
                        (unsigned long)(per ? wos_up_ticks[6] / per : 0), (unsigned long)(per ? wos_up_ticks[7] / per : 0));
                for (k = 0; k < 8; k++) wos_up_ticks[k] = 0; }
              for (k = 0; k < 8; k++) wos_up_texels[k] = wos_up_calls[k] = 0; }
            { extern unsigned int wos_tex_compress, wos_tex_gc, wos_tex_clearall;
              extern uint32_t wos_tex_compress_tb;
              wos_log("[prof] texture cache in %lu frames: %u compactions (%lu us each), %u garbage collections, %u clear-alls",
                      (unsigned long)s_stat_frames, wos_tex_compress,
                      (unsigned long)(wos_tex_compress ? (uint64_t)wos_tex_compress_tb * 1000000u / wos_tb_hz() / wos_tex_compress : 0),
                      wos_tex_gc, wos_tex_clearall);
              wos_tex_compress = wos_tex_gc = wos_tex_clearall = 0; wos_tex_compress_tb = 0; }
        }
        wos_log("[prof] UploadScreen %lu us, %lu textures made, %lu cd fills; SysUpdate %lu us (input %lu)",
                st[WOS_PROF_UPSCREEN], (unsigned long)(wos_prof_reads >> 16),
                (unsigned long)((wos_prof_reads >> 8) & 0xff),
                st[WOS_PROF_SYS], st[WOS_PROF_INPUT]);
        wos_prof_reads = 0;
        {
            extern uint32_t wos_cda_jumps, wos_cda_waits, wos_cda_jump_tb, wos_cda_wait_tb;
            uint32_t per = wos_tb_hz() / 1000u;
            extern uint32_t wos_cda_age_tb, wos_cda_age_max, wos_cda_lat_tb, wos_cda_lat_max;
            wos_log("[prof] cd image in %lu frames: %lu jumps (%lu ms), %lu waits for the read-ahead (%lu ms); "
                    "read age when needed %lu ms avg %lu max, done after %lu ms avg %lu max",
                    (unsigned long)s_stat_frames, (unsigned long)wos_cda_jumps,
                    (unsigned long)(wos_cda_jump_tb / per), (unsigned long)wos_cda_waits,
                    (unsigned long)(wos_cda_wait_tb / per),
                    (unsigned long)(wos_cda_waits ? wos_cda_age_tb / per / wos_cda_waits : 0),
                    (unsigned long)(wos_cda_age_max / per),
                    (unsigned long)(wos_cda_waits ? wos_cda_lat_tb / per / wos_cda_waits : 0),
                    (unsigned long)(wos_cda_lat_max / per));
            {
                extern uint32_t wos_cda_arrived, wos_cda_arrived_tb;
                extern uint32_t wos_cda_t_wait, wos_cda_t_get, wos_cda_t_inv;
                uint32_t pu = per / 1000u;
                wos_log("[prof] cd image: %lu read-aheads already there when needed (%lu us each to collect); "
                        "in 300 frames WaitPort %lu us, GetMsg %lu us, buffer invalidate %lu us",
                        (unsigned long)wos_cda_arrived,
                        (unsigned long)(wos_cda_arrived ? wos_cda_arrived_tb / pu / wos_cda_arrived : 0),
                        (unsigned long)(wos_cda_t_wait / pu), (unsigned long)(wos_cda_t_get / pu),
                        (unsigned long)(wos_cda_t_inv / pu));
                wos_cda_arrived = wos_cda_arrived_tb = 0;
                wos_cda_t_wait = wos_cda_t_get = wos_cda_t_inv = 0;
            }
            wos_cda_jumps = wos_cda_waits = wos_cda_jump_tb = wos_cda_wait_tb = 0;
            wos_cda_age_tb = wos_cda_age_max = wos_cda_lat_tb = wos_cda_lat_max = 0;
        }
        {
            extern unsigned int wos_au_fed, wos_au_under, wos_au_min, wos_au_short, wos_au_feeds;
            wos_log("[audio] %lu frames fed in %lu ms (%lu feeds, %lu short), %lu underruns, least queued %ld",
                    (unsigned long)wos_au_fed, (unsigned long)(span / 1000), (unsigned long)wos_au_feeds,
                    (unsigned long)wos_au_short, (unsigned long)wos_au_under, (long)(int)wos_au_min);
            wos_au_fed = wos_au_under = wos_au_short = wos_au_feeds = 0;
            wos_au_min = ~0u;
        }
        {
            extern unsigned int wos_bt_calls, wos_bt_full, wos_xa_sectors;
            wos_log("[prof] branch tests per frame %lu, %lu of them full; xa sectors %lu played, %lu refused",
                    (unsigned long)(wos_bt_calls / s_stat_frames),
                    (unsigned long)(wos_bt_full / s_stat_frames),
                    (unsigned long)(wos_xa_sectors & 0xffff), (unsigned long)(wos_xa_sectors >> 16));
            wos_bt_calls = wos_bt_full = wos_xa_sectors = 0;
            {
                extern unsigned int wos_idle_found, wos_idle_skips;

                wos_log("[prof] idle loops found %u, skips per frame %lu; now %08lx<-%08lx %lu, %08lx<-%08lx %lu, %08lx<-%08lx %lu, %08lx<-%08lx %lu",
                        wos_idle_found, (unsigned long)(wos_idle_skips / s_stat_frames),
                        (unsigned long)wos_idle[0].pc, (unsigned long)wos_idle[0].pred, (unsigned long)wos_idle[0].skips,
                        (unsigned long)wos_idle[1].pc, (unsigned long)wos_idle[1].pred, (unsigned long)wos_idle[1].skips,
                        (unsigned long)wos_idle[2].pc, (unsigned long)wos_idle[2].pred, (unsigned long)wos_idle[2].skips,
                        (unsigned long)wos_idle[3].pc, (unsigned long)wos_idle[3].pred, (unsigned long)wos_idle[3].skips);
                { int k; for (k = 0; k < 4; k++) wos_idle[k].skips = 0; }
                wos_idle_skips = 0;
                {
                    extern u32 wos_idle_why[5], wos_irq_taken[11];
                    wos_log("[prof] idle arrivals a frame: due %lu, other pred %lu, round changed %lu, branch changed %lu, skipped %lu; irqs taken a frame by bit: vbl %lu gpu %lu cd %lu dma %lu t0 %lu t1 %lu t2 %lu pad %lu sio %lu spu %lu pio %lu",
                            (unsigned long)(wos_idle_why[0] / s_stat_frames), (unsigned long)(wos_idle_why[1] / s_stat_frames),
                            (unsigned long)(wos_idle_why[2] / s_stat_frames), (unsigned long)(wos_idle_why[3] / s_stat_frames),
                            (unsigned long)(wos_idle_why[4] / s_stat_frames),
                            (unsigned long)(wos_irq_taken[0] / s_stat_frames), (unsigned long)(wos_irq_taken[1] / s_stat_frames),
                            (unsigned long)(wos_irq_taken[2] / s_stat_frames), (unsigned long)(wos_irq_taken[3] / s_stat_frames),
                            (unsigned long)(wos_irq_taken[4] / s_stat_frames), (unsigned long)(wos_irq_taken[5] / s_stat_frames),
                            (unsigned long)(wos_irq_taken[6] / s_stat_frames), (unsigned long)(wos_irq_taken[7] / s_stat_frames),
                            (unsigned long)(wos_irq_taken[8] / s_stat_frames), (unsigned long)(wos_irq_taken[9] / s_stat_frames),
                            (unsigned long)(wos_irq_taken[10] / s_stat_frames));
                    memset(wos_idle_why, 0, sizeof wos_idle_why);
                    memset(wos_irq_taken, 0, sizeof wos_irq_taken);
                }
            }
            {
                extern unsigned int wos_xa_why[6];
                wos_log("[prof] not xa: no strsnd %u, not form2 audio %u, filter %u, chan 0x80 %u, other chan %u, xa off %u",
                        wos_xa_why[0], wos_xa_why[1], wos_xa_why[2], wos_xa_why[3], wos_xa_why[4], wos_xa_why[5]);
                memset(wos_xa_why, 0, sizeof wos_xa_why);
                {
                    extern unsigned int wos_xa_rip[3];
                    wos_log("[prof] form-1 rip audio: %u sectors rebuilt, %u waiting for the spacing, %u at another spacing",
                            wos_xa_rip[0], wos_xa_rip[1], wos_xa_rip[2]);
                    memset(wos_xa_rip, 0, sizeof wos_xa_rip);
                }
                {
                    extern unsigned int wos_vlc_calls, wos_vlc_bad, wos_vlc_instr;
                    if (wos_vlc_calls)
                        wos_log("[prof] movie decoder as C: %u calls, %u instructions each, %lu us a frame, %u differing from its code",
                                wos_vlc_calls, wos_vlc_instr / wos_vlc_calls, st[WOS_PROF_VLC], wos_vlc_bad);
                    wos_vlc_calls = wos_vlc_instr = 0;
                }
#ifdef WOS_PROFCPU
                {
                    extern unsigned int wos_gte_ops, wos_hw_ops;
                    wos_log("[prof] cpu split: gte %lu us (%u ops), hardware registers %lu us (%u accesses) a frame",
                            st[WOS_PROF_GTE], wos_gte_ops / s_stat_frames, st[WOS_PROF_HW], wos_hw_ops / s_stat_frames);
                    wos_gte_ops = wos_hw_ops = 0;
                }
                {
                    extern void wos_gte_report(char *line, int size, unsigned frames);
                    char gl[400];
                    wos_gte_report(gl, sizeof gl, s_stat_frames);
                    wos_log("[prof] gte ops (share of gte time, count a frame):%s", gl);
                }
                {
                    extern void wos_bprof_report(void);
                    wos_bprof_report();
                }
#endif
                {
                    extern unsigned int wos_rec_blocks, wos_rec_flushes;
                    wos_log("[prof] dynarec: %lu blocks made a frame in %lu us, %u full flushes",
                            (unsigned long)(wos_rec_blocks / s_stat_frames), st[WOS_PROF_REC], wos_rec_flushes);
                    wos_rec_blocks = wos_rec_flushes = 0;
                }
                {
                    extern unsigned int wos_st_polls, wos_st_skips, wos_ev_why[32];
                    char line[200];
                    int i, len = 0, top[4], n, j;
                    for (n = 0; n < 4; n++) {
                        int best = -1;
                        for (i = 0; i < 32; i++) {
                            for (j = 0; j < n && top[j] != i; j++)
                                ;
                            if (j == n && wos_ev_why[i] && (best < 0 || wos_ev_why[i] > wos_ev_why[best]))
                                best = i;
                        }
                        if (best < 0)
                            break;
                        top[n] = best;
                        len += snprintf(line + len, sizeof line - len, " %d:%lu", best,
                                        (unsigned long)(wos_ev_why[best] / s_stat_frames));
                    }
                    line[len] = 0;
                    extern unsigned int wos_rw_rounds, wos_gq_polls, wos_gq_skips;
                    wos_log("[prof] movie polls %u, %u skipped to the next event; gpu queue polls %u, %u skipped; vblank wait rounds as C %u; next event set by (0 counters, 1 irq, 2+int) a frame:%s",
                            wos_st_polls, wos_st_skips, wos_gq_polls, wos_gq_skips, wos_rw_rounds, line);
                    wos_st_polls = wos_st_skips = wos_rw_rounds = wos_gq_polls = wos_gq_skips = 0;
                    memset(wos_ev_why, 0, sizeof wos_ev_why);
                }
            }
        }
        wos_log("[stats] %lu.%02lu fps, %lu presents, %lu late; per frame: upload %lu us, swap %lu us, wait %lu us",
                fps100 / 100, fps100 % 100,
                (unsigned long)s_stat_presents, (unsigned long)s_stat_late,
                (unsigned long)(s_stat_upload_us / s_stat_frames),
                (unsigned long)(s_stat_swap_us / s_stat_frames),
                (unsigned long)(s_stat_wait_us / s_stat_frames));
    }
    s_stat_start_us = now;
    wos_log("[stats] busy ms a frame: <14 %lu, 14 %lu, 15 %lu, 16 %lu, 16.7 %lu, 17 %lu, 18 %lu, 19 %lu, 20-24 %lu, 25+ %lu; max %lu us",
            (unsigned long)s_busy_hist[0], (unsigned long)s_busy_hist[1], (unsigned long)s_busy_hist[2],
            (unsigned long)s_busy_hist[3], (unsigned long)s_busy_hist[4], (unsigned long)s_busy_hist[5],
            (unsigned long)s_busy_hist[6], (unsigned long)s_busy_hist[7], (unsigned long)s_busy_hist[8],
            (unsigned long)s_busy_hist[9], (unsigned long)s_busy_max);
    memset(s_busy_hist, 0, sizeof s_busy_hist);
    s_busy_max = 0;
    s_stat_frames = s_stat_presents = s_stat_late = 0;
    s_stat_upload_us = s_stat_swap_us = s_stat_wait_us = 0;
}
