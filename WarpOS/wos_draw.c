/*
 * wos_draw.c -- what SoftGPU/drawGX.c is on the Wii, for the WarpOS build:
 * the software GPUs' display output (the rearmed vout callbacks and the old
 * soft GPU's DoBufferSwap), and the frame limiter, over wos_video.c.
 *
 * Copyright (C) 1999-2003 Pete Bernert (the P.E.Op.S. frame limiter)
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "../coredebug.h"
#include "../gpulib/stdafx.h"
#include "../SoftGPU/externals.h"
#include "../SoftGPU/gpu.h"
#include "../SoftGPU/draw.h"
#include "../Gamecube/DEBUG.h"
#include "../Gamecube/wiiSXconfig.h"
#include "../psxcounters.h"
#include "../gpulib/plugin_lib.h"

#include "wos_video.h"
#include "wos_sys.h"

// misc globals (drawGX.c's)
int            iResX;
int            iResY;
int            iDesktopCol=16;
int            iShowFPS=1;
int            iUseNoStretchBlt=0;
int            iFastFwd=0;
int            iDebugMode=0;
int            iFVDisplay=0;
PSXPoint_t     ptCursorPoint[8];
unsigned short usCursorActive=0;
int            backFromMenu=0;

#define RESX_MAX 1024	//Vmem width
#define RESY_MAX 512	//Vmem height
int		iResX_Max=RESX_MAX;
int		iResY_Max=RESY_MAX;

char *	pCaptionText;
char fpsInfo[32];

static int screen_w, screen_h;

// The GX path's hooks: the hardware GPU and the menu call these on the Wii.
void gc_vout_render(void)      { wos_video_swap(); }
/* GlesGpu's present. On the Wii the EFB keeps its pixels after the display
 * copy unless asked to clear; MiniGL's GPU-side present copy
 * (MGLPPC_CPPRESENT=1) keeps the back buffer the same way. */
int  gx_vout_render(short canSwapFrameBuf)
{
	wos_video_swap();
#ifdef WOS_HAVE_GLES
	if (canSwapFrameBuf) {
		extern void wosgl_ClearFramebuffer(void);
		wosgl_ClearFramebuffer();
	}
#endif
	return 1;
}
void gx_vout_wait_idle(void)   {}
void gc_vout_disabled(void)    { wos_video_present_blank(); }
void showFpsAndDebugInfo(void) {}
void switchToTVMode(short dWidth, short dHeight, bool retMenu) { (void)dWidth; (void)dHeight; (void)retMenu; }

int gc_vout_open(void) { return 0; }
int gx_vout_open(void) { return 0; }
static void gc_vout_close(void) {}

static void gc_vout_flip(const void *vram, int stride, int bgr24,
			      int x, int y, int w, int h, int dims_changed) {
	(void)x; (void)y; (void)dims_changed;
	if (vram == NULL) {
		wos_video_present_blank();
		return;
	}
	wos_video_present_vram(vram, stride * 2, bgr24, w, h);
}

// For old softGpu
void DoBufferSwap(void)                                // SWAP BUFFERS
{                                                      // (we don't swap... we blit only)
	long x = PSXDisplay.DisplayPosition.x;
	long y = PSXDisplay.DisplayPosition.y;
	short iDX = PreviousPSXDisplay.Range.x1 & 0xFFF8;
	if (iDX < PreviousPSXDisplay.Range.x1)
		iDX += 8;
	short iDY = PreviousPSXDisplay.DisplayMode.y;

	u8 *imgPtr = (u8*)(psxVuw + ((y<<10) + x));
	if(PSXDisplay.RGB24) {
		long lPitch=iResX_Max<<1;

		if(PreviousPSXDisplay.Range.y0)                       // centering needed?
		{
			imgPtr+=PreviousPSXDisplay.Range.y0*lPitch;
			iDY-=PreviousPSXDisplay.Range.y0;
		}
		imgPtr+=PreviousPSXDisplay.Range.x0<<1;
	}

	wos_video_present_vram(imgPtr, iResX_Max*2, PSXDisplay.RGB24, iDX, iDY);
}

// For old softGpu
void DoClearFrontBuffer(void)                          // CLEAR DX BUFFER
{
	wos_video_present_blank();
}

static void gc_vout_set_mode(int w, int h, int raw_w, int raw_h, int bpp) {
	(void)w; (void)h; (void)bpp;
	screen_w = raw_w;
	screen_h = raw_h;
}

extern u32 hSyncCount;
extern u32 frame_counter;
extern void gpu_state_change(int what, int cycles);

static struct rearmed_cbs gc_rearmed_cbs = {
   .pl_vout_open     = gc_vout_open,
   .pl_vout_set_mode = gc_vout_set_mode,
   .pl_vout_flip     = gc_vout_flip,
   .pl_vout_close    = gc_vout_close,
   /* from psxcounters */
   .gpu_hcnt         = &hSyncCount,
   .gpu_frame_count  = &frame_counter,
   .gpu_state_change = gpu_state_change,

   .gpu_unai = {
	   .lighting = 1,
	   .blending = 1,
   },
};

// Frame limiting, taken from plugin_lib.c as drawGX.c has it.
extern int g_emu_resetting;
void pl_chg_psxtype(int is_pal_);
static int is_pal, frame_interval, frame_interval1024;

static float  fps_cur  = 0;

static void CalcFps(void)
{
	static uint64_t last_us;
	static unsigned long fps_cnt = 0;
	static uint64_t fps_us = 0;
	uint64_t now = wos_now_us();

	fps_us += now - last_us;
	last_us = now;
	if (++fps_cnt == 10)
	{
		fps_cur = fps_us ? (float)(10 * 1000000.0 / (double)fps_us) : 0.0f;
		fps_cnt = 0;
		fps_us = 0;
	}
	sprintf(fpsInfo, "FPS %.2f", fps_cur);
}

void CheckFrameRate(void)
{
	if (frameLimit[0] == FRAMELIMIT_AUTO)
		wos_video_limit(gc_rearmed_cbs.gpu_peops.fFrameRateHz);
	if (showFPSonScreen == FPS_SHOW)
		CalcFps();
}

/* called on every vsync */
void pl_frame_limit(void)
{
	if (g_emu_resetting)
		return;

	// used by P.E.Op.S. frame limit code
	CheckFrameRate();
}

double max_vid_fps;

void pl_timing_prepare(int is_pal_)
{
	gc_rearmed_cbs.fskip_advice = 0;
	gc_rearmed_cbs.flips_per_sec = 0;
	gc_rearmed_cbs.cpu_usage = 0;

	pl_chg_psxtype(is_pal_);
}

void pl_chg_psxtype(int is_pal_)
{
	is_pal = is_pal_;
	max_vid_fps = psxGetFps();
	frame_interval = (int)(1000000.0 / max_vid_fps);
	frame_interval1024 = (int)(1000000.0 * 1024.0 / max_vid_fps);

	// used by P.E.Op.S. frame limit code
	if (PSXDisplay.Interlaced)
	{
		gc_rearmed_cbs.gpu_peops.fFrameRateHz = is_pal ? 50.00238f : 59.94146f;
	}
	else
	{
		gc_rearmed_cbs.gpu_peops.fFrameRateHz = (float)max_vid_fps;
	}

	gc_rearmed_cbs.gpu_peops.dwFrameRateTicks =
		(int) (TIMEBASE / gc_rearmed_cbs.gpu_peops.fFrameRateHz);
}

void plugin_call_rearmed_cbs(unsigned long autoDwActFixes, int cfgUseDithering)
{
	gc_rearmed_cbs.screen_centering_type_default =
		Config.hacks.gpu_centering ? 1 : 0;

	extern void LIB_GPUrearmedCallbacks(const struct rearmed_cbs *cbs);
	gc_rearmed_cbs.gpu_peops.dwActFixes = autoDwActFixes;
	gc_rearmed_cbs.gpu_peops.iUseDither = cfgUseDithering;
	gc_rearmed_cbs.frameskip = frameSkip;

	LIB_GPUrearmedCallbacks(&gc_rearmed_cbs);
}
