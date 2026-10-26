/*
 * wos_video.h -- the display for the WarpOS build: a MiniGL (R200) context
 * on its own screen or window.
 */
#ifndef WOS_VIDEO_H
#define WOS_VIDEO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opens MiniGL and its context. Size and mode from ENV:
 * WSX_WIDTH/WSX_HEIGHT (640x480), WSX_WINDOW=1 for a Workbench window,
 * WSX_VSYNC=1 to wait for the display at every swap. 0 on success. */
int   wos_video_open(void);
void  wos_video_close(void);

/* The context's window (a struct Window *), for its IDCMP port. */
void *wos_video_window(void);
int   wos_video_width(void);
int   wos_video_height(void);

/* The software GPUs' finished frame: PSX VRAM rows (little-endian BGR555, or
 * packed RGB888 when rgb24), `pitch` bytes apart, shown scaled to the display. */
void  wos_video_present_vram(const void *src, int pitch, int rgb24, int w, int h);

/* A black frame (display disabled). */
void  wos_video_present_blank(void);

/* The hardware GPU drew a frame into the context: show it. */
void  wos_video_swap(void);

/* Waits until the PSX frame period since the last call has passed
 * (the frame limiter); `hz` is the PSX refresh. */
void  wos_video_limit(float hz);

/* Once a PSX frame: counts frames and logs a stats line every 300. */
void  wos_video_frame_stats(void);

#ifdef __cplusplus
}
#endif

#endif
