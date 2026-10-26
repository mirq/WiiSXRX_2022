/*
 * wos_gx.h -- the few libogc GX calls GlesGpu makes, for the WarpOS build.
 *
 * GlesGpu (gpuPlugin.c and the files it #includes, one translation unit)
 * reads the Wii's embedded framebuffer back with EFB-to-texture copies, and
 * decodes the copies itself: 4x4-texel tiles of RGB5A3 halfwords. Here a copy
 * is a synchronous glReadPixels that writes that same layout
 * (wosgl_CopyFramebufferGX), so the readback state machine
 * (gpuVramReadback.inc) is unchanged. Everything else -- display copies,
 * copy filters, draw syncs -- is a no-op or completes at once.
 */
#ifndef WOS_GX_H
#define WOS_GX_H

#include <stdlib.h>
#include <malloc.h>
#include "../WarpOS/compat/gctypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Copy the framebuffer rectangle (top-left origin) into dst as GX RGB5A3
 * tiles of dstW x dstH texels; scale 2 is the 2:1 box filter. */
int wosgl_CopyFramebufferGX(int x, int y, int w, int h,
                            int dstW, int dstH, int scale, void *dst);

/* drawGX.c's hooks, which WarpOS/wos_draw.c provides. */
void switchToTVMode(short dWidth, short dHeight, bool retMenu);
int  gx_vout_open(void);
void OldGpuCheckFrameRate(void);

#define GX_FALSE     0
#define GX_TRUE      1
#define GX_DISABLE   0
#define GX_ENABLE    1
#define GX_TF_RGB5A3 0x5

/* The video mode GlesGpu consults: the EFB is the MiniGL context. */
typedef struct {
    u32 viTVMode;
    u16 fbWidth, efbHeight, xfbHeight;
    u16 viXOrigin, viYOrigin, viWidth, viHeight;
    u32 xfbMode;
    u8  field_rendering, aa;
    u8  sample_pattern[12][2];
    u8  vfilter[7];
} GXRModeObj;

/* One translation unit includes this header (GlesGpu/gpuPlugin.c): the
 * mode is defined here, 640x480 like the context GlesGpu draws into. */
static GXRModeObj s_wos_vmode = { 0, 640, 480, 480, 0, 0, 640, 480, 0, 0, 0 };
GXRModeObj *vmode = &s_wos_vmode;

#define MEM_K0_TO_K1(x) ((void *)(x))

static inline void *_mem2_malloc(u32 size)  { return memalign(32, size); }
static inline void  _mem2_free(void *p)     { free(p); }
static inline void  DCInvalidateRange(void *p, u32 n) { (void)p; (void)n; }
static inline void  DCFlushRange(void *p, u32 n)      { (void)p; (void)n; }

static inline u32 GX_GetTexBufferSize(u16 w, u16 h, u32 fmt, u8 mipmap, u8 maxlod)
{
    (void)fmt; (void)mipmap; (void)maxlod;
    return (u32)((w + 3) & ~3) * ((h + 3) & ~3) * 2;
}

static int s_gx_copy_x, s_gx_copy_y, s_gx_copy_w, s_gx_copy_h;
static int s_gx_dst_w, s_gx_dst_h, s_gx_dst_half;

static inline void GX_SetTexCopySrc(u16 x, u16 y, u16 w, u16 h)
{
    s_gx_copy_x = x; s_gx_copy_y = y; s_gx_copy_w = w; s_gx_copy_h = h;
}

static inline void GX_SetTexCopyDst(u16 w, u16 h, u32 fmt, u8 mipmap)
{
    (void)fmt;
    s_gx_dst_w = w; s_gx_dst_h = h; s_gx_dst_half = mipmap ? 1 : 0;
}

static inline void GX_CopyTex(void *dest, u8 clear)
{
    (void)clear;
    wosgl_CopyFramebufferGX(s_gx_copy_x, s_gx_copy_y, s_gx_copy_w, s_gx_copy_h,
                            s_gx_dst_w, s_gx_dst_h, s_gx_dst_half ? 2 : 1, dest);
}

/* Draw syncs: the copy above has completed when it returns, so the callback
 * runs at once. */
typedef void (*GXDrawSyncCallback)(u16 token);
static GXDrawSyncCallback s_gx_sync_cb;
static inline GXDrawSyncCallback GX_SetDrawSyncCallback(GXDrawSyncCallback cb)
{
    GXDrawSyncCallback old = s_gx_sync_cb;
    s_gx_sync_cb = cb;
    return old;
}
static inline void GX_SetDrawSync(u16 token)
{
    if (s_gx_sync_cb)
        s_gx_sync_cb(token);
}

static inline void GX_SetCopyFilter(u8 aa, u8 sample_pattern[12][2], u8 vf, u8 *vfilter)
{
    (void)aa; (void)sample_pattern; (void)vf; (void)vfilter;
}
static inline void  GX_PixModeSync(void) {}
static inline void  GX_Flush(void) {}
static inline void  GX_DrawDone(void) {}
static inline f32   GX_GetYScaleFactor(u16 efbHeight, u16 xfbHeight) { (void)efbHeight; (void)xfbHeight; return 1.0f; }
static inline u32   GX_SetDispCopyYScale(f32 yscale) { (void)yscale; return vmode ? vmode->xfbHeight : 480; }
static inline void  GX_SetScissor(u32 x, u32 y, u32 w, u32 h) { (void)x; (void)y; (void)w; (void)h; }
static inline void  GX_SetDispCopySrc(u16 x, u16 y, u16 w, u16 h) { (void)x; (void)y; (void)w; (void)h; }
static inline void  GX_SetDispCopyDst(u16 w, u16 h) { (void)w; (void)h; }
static inline void  GX_SetFieldMode(u8 field_mode, u8 half_aspect) { (void)field_mode; (void)half_aspect; }

#ifdef __cplusplus
}
#endif

#endif
