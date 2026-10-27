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
#include "wos_amiga.h"

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

int wos_video_open(void)
{
    int windowed = wos_env_int("WSX_WINDOW", 0);

    s_width    = wos_env_int("WSX_WIDTH", 640);
    s_height   = wos_env_int("WSX_HEIGHT", 480);
    s_bilinear = wos_env_int("WSX_BILINEAR", 1);

    if (!MiniGLOpen()) {
        wos_log("MiniGLOpen failed: is minigl_ppc.dll in PROGDIR: and minigl_ppc_host running?");
        return -1;
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
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    mglSwitchDisplay();

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

void wos_video_present_vram(const void *src, int pitch, int rgb24, int w, int h)
{
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
    if (!s_open)
        return;
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    wos_video_swap();
}

void wos_video_swap(void)
{
    uint64_t t0;

    if (!s_open)
        return;
    WOS_CRUMB(21);
    t0 = wos_now_us();
    mglSwitchDisplay();
    s_stat_swap_us += wos_now_us() - t0;
    s_stat_presents++;
}

/* ---- Pacing ------------------------------------------------------------ */

void wos_video_limit(float hz)
{
    uint64_t period, now;

    if (hz < 1.0f)
        return;
    period = (uint64_t)(1000000.0f / hz);
    now = wos_now_us();
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
}

void wos_video_frame_stats(void)
{
    uint64_t now, span;

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
        /* rcnt holds lace and spu; irq holds cd */
        wos_log("[prof] us per frame %lu: gpu %lu, rcnt %lu (lace/present %lu, spu %lu), irq %lu (cd read %lu), mdec %lu, the rest (cpu) %lu",
                (unsigned long)(span / s_stat_frames), st[WOS_PROF_GPU], st[WOS_PROF_RCNT],
                st[WOS_PROF_LACE], st[WOS_PROF_SPU], st[WOS_PROF_IRQ], st[WOS_PROF_CDR],
                st[WOS_PROF_MDEC],
                (unsigned long)(span / s_stat_frames) - st[WOS_PROF_GPU] - st[WOS_PROF_RCNT]
                    - st[WOS_PROF_IRQ] - st[WOS_PROF_MDEC] - st[WOS_PROF_GP1]);
        wos_log("[prof] gp1 %lu us, framebuffer reads %lu us (%lu reads), textures: convert %lu us, upload %lu us",
                st[WOS_PROF_GP1], st[WOS_PROF_READ], (unsigned long)(wos_prof_reads & 0xff),
                st[WOS_PROF_TEXCONV], st[WOS_PROF_TEXUP]);
        wos_log("[prof] UploadScreen %lu us, %lu textures made, %lu cd fills; SysUpdate %lu us (input %lu)",
                st[WOS_PROF_UPSCREEN], (unsigned long)(wos_prof_reads >> 16),
                (unsigned long)((wos_prof_reads >> 8) & 0xff),
                st[WOS_PROF_SYS], st[WOS_PROF_INPUT]);
        wos_prof_reads = 0;
        {
            extern unsigned int wos_bt_calls, wos_bt_full, wos_xa_sectors;
            wos_log("[prof] branch tests per frame %lu, %lu of them full; xa sectors %lu played, %lu refused",
                    (unsigned long)(wos_bt_calls / s_stat_frames),
                    (unsigned long)(wos_bt_full / s_stat_frames),
                    (unsigned long)(wos_xa_sectors & 0xffff), (unsigned long)(wos_xa_sectors >> 16));
            wos_bt_calls = wos_bt_full = wos_xa_sectors = 0;
            {
                extern unsigned int wos_idle_found, wos_idle_skips;
                wos_log("[prof] idle loops found %u, skips per frame %lu",
                        wos_idle_found, (unsigned long)(wos_idle_skips / s_stat_frames));
                wos_idle_skips = 0;
            }
            {
                extern unsigned int wos_xa_why[6];
                wos_log("[prof] not xa: no strsnd %u, not form2 audio %u, filter %u, chan 0x80 %u, other chan %u, xa off %u",
                        wos_xa_why[0], wos_xa_why[1], wos_xa_why[2], wos_xa_why[3], wos_xa_why[4], wos_xa_why[5]);
                memset(wos_xa_why, 0, sizeof wos_xa_why);
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
    s_stat_frames = s_stat_presents = s_stat_late = 0;
    s_stat_upload_us = s_stat_swap_us = s_stat_wait_us = 0;
}
