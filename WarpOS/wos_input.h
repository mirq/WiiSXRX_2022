/*
 * wos_input.h -- keyboard and joystick ports for the WarpOS build.
 */
#ifndef WOS_INPUT_H
#define WOS_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PSX buttons, active low as the pad sends them: bits 0..7 are the first data
 * byte of a poll, bits 8..15 the second. */
enum {
    PSXB_SELECT = 1 << 0,  PSXB_L3     = 1 << 1,  PSXB_R3     = 1 << 2,
    PSXB_START  = 1 << 3,  PSXB_UP     = 1 << 4,  PSXB_RIGHT  = 1 << 5,
    PSXB_DOWN   = 1 << 6,  PSXB_LEFT   = 1 << 7,
    PSXB_L2     = 1 << 8,  PSXB_R2     = 1 << 9,  PSXB_L1     = 1 << 10,
    PSXB_R1     = 1 << 11, PSXB_TRIANGLE = 1 << 12, PSXB_CIRCLE = 1 << 13,
    PSXB_CROSS  = 1 << 14, PSXB_SQUARE = 1 << 15
};

typedef struct {
    int      connected;
    uint16_t buttons;          /* active low */
    uint8_t  lx, ly, rx, ry;   /* 0..255, 128 centre */
} WosPadState;

/* What the user asked for besides the pads (a bit mask). */
enum {
    WOS_ACT_QUIT      = 1 << 0,
    WOS_ACT_SAVESTATE = 1 << 1,
    WOS_ACT_LOADSTATE = 1 << 2,
    WOS_ACT_FASTFWD   = 1 << 3     /* toggles the frame limiter */
};

void wos_input_init(void);
void wos_input_shutdown(void);

/* Once a frame: the window's messages (only when its signal is set), CTRL-C,
 * and the joystick ports. `window` is a struct Window *, or NULL. Returns the
 * WOS_ACT_* actions requested since the last call. */
unsigned wos_input_poll(void *window);

/* The state the PSX pad `pad` (0 or 1) shows; called by wos_pad.c's poll. */
void wos_input_read_pad(int pad, WosPadState *st);

/* Rebind a PSX button (one PSXB_* bit) to an Amiga raw key code (0..127). */
void wos_input_bind_key(uint16_t psx_button, int rawkey);

#ifdef __cplusplus
}
#endif

#endif
