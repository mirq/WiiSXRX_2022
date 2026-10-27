/*
 * wos_input.c -- the PSX pads from the Amiga's keyboard and joystick ports.
 *
 * Keyboard: IDCMP_RAWKEY from the display's window, raw Amiga scancodes with
 * bit 7 (IECODE_UP_PREFIX) on release. The port is drained every frame, as
 * quake3amiga does: its signal bit belongs to the task that opened the
 * window, which is MiniGL's 68k host, so SetSignal here never shows it.
 * CTRL-C still comes as a signal. Joysticks and CD32 pads: lowlevel.library ReadJoyPort. Each read is
 * a trip to the 68k, so a port is read every frame only while something
 * answers in it; an empty one is looked at again about once a second.
 *
 * PSX pad 1 is the keyboard and joystick port 1; pad 2 is joystick port 0
 * (the mouse port), present only while a stick or pad answers there.
 */
#include <string.h>

#include "wos_input.h"
#include "wos_sys.h"
#include "wos_amiga.h"

#define AKEY_ESC      0x45
#define AKEY_F5       0x54
#define AKEY_F7       0x56
#define AKEY_F10      0x59
#define AKEY_UP_PREFIX 0x80

/* The keyboard's PSX bindings: raw key -> PSXB_* bit. */
static uint16_t s_keymap[128];
static uint8_t  s_keydown[128];
/* Frames a pressed key still reads as held after its release: the port is
 * drained every few frames, and a tap can come and go inside one drain. */
#define KEY_LATCH 4
static uint8_t  s_keylatch[128];
static unsigned s_peek_drains, s_timer_drains, s_timer_mail;

/* Joystick ports: live (a stick or pad answers) and when to look again. */
static int      s_port_live[2];
static int      s_port_probe[2];
static uint16_t s_port_buttons[2];     /* active HIGH while held */

static void set_default_keys(void)
{
    memset(s_keymap, 0, sizeof(s_keymap));
    s_keymap[0x4C] = PSXB_UP;
    s_keymap[0x4D] = PSXB_DOWN;
    s_keymap[0x4E] = PSXB_RIGHT;
    s_keymap[0x4F] = PSXB_LEFT;
    s_keymap[0x32] = PSXB_CROSS;      /* X */
    s_keymap[0x33] = PSXB_CIRCLE;     /* C */
    s_keymap[0x31] = PSXB_SQUARE;     /* Z */
    s_keymap[0x21] = PSXB_TRIANGLE;   /* S */
    s_keymap[0x10] = PSXB_L1;         /* Q */
    s_keymap[0x11] = PSXB_R1;         /* W */
    s_keymap[0x01] = PSXB_L2;         /* 1 */
    s_keymap[0x02] = PSXB_R2;         /* 2 */
    s_keymap[0x44] = PSXB_START;      /* RETURN */
    s_keymap[0x43] = PSXB_START;      /* ENTER */
    s_keymap[0x40] = PSXB_SELECT;     /* SPACE */
}

/* Off by default, as quake3amiga's in_joystick: a ReadJoyPort is a 68k trip of
 * about 5 ms on the A4000. SetEnv WSX_JOYPORTS 1 reads one port a frame. */
static int s_joyports;

void wos_input_init(void)
{
    s_joyports = wos_env_int("WSX_JOYPORTS", 0);
    set_default_keys();
    memset(s_keydown, 0, sizeof(s_keydown));
    memset(s_keylatch, 0, sizeof(s_keylatch));
    memset(s_port_live, 0, sizeof(s_port_live));
    memset(s_port_probe, 0, sizeof(s_port_probe));
    memset(s_port_buttons, 0, sizeof(s_port_buttons));
}

void wos_input_shutdown(void)
{
}

void wos_input_bind_key(uint16_t psx_button, int rawkey)
{
    int i;
    if (rawkey < 0 || rawkey > 127)
        return;
    for (i = 0; i < 128; i++)
        if (s_keymap[i] == psx_button)
            s_keymap[i] = 0;
    s_keymap[rawkey] = psx_button;
}

static uint16_t keyboard_buttons(void)
{
    uint16_t b = 0;
    int i;
    for (i = 0; i < 128; i++)
        if (s_keydown[i] || s_keylatch[i])
            b |= s_keymap[i];
    return b;
}

static unsigned handle_key(UWORD code, UWORD qual)
{
    int down = !(code & AKEY_UP_PREFIX);
    int key  = code & 0x7F;

    s_keydown[key] = (uint8_t)down;
    if (down)
        s_keylatch[key] = KEY_LATCH;
    if (!down || (qual & IEQUALIFIER_REPEAT))
        return 0;
    switch (key) {
    case AKEY_ESC: return WOS_ACT_QUIT;
    case AKEY_F5:  return WOS_ACT_SAVESTATE;
    case AKEY_F7:  return WOS_ACT_LOADSTATE;
    case AKEY_F10: return WOS_ACT_FASTFWD;
    }
    return 0;
}

static void read_port(ULONG port, int slot)
{
    ULONG s, type;
    uint16_t b = 0;
    uint64_t t0;
    uint32_t us;

    if (!LowLevelBase)
        return;
    if (!s_port_live[slot] && s_port_probe[slot]-- > 0)
        return;
    s_port_probe[slot] = 60;

    t0 = wos_now_us();
    s = ReadJoyPort(port);
    us = (uint32_t)(wos_now_us() - t0);
    if (us > 3000)
        wos_log("[joy] ReadJoyPort(%lu) took %lu us", (unsigned long)port, (unsigned long)us);

    type = s & JP_TYPE_MASK;
    s_port_live[slot] = (type == JP_TYPE_JOYSTK || type == JP_TYPE_GAMECTLR);
    if (!s_port_live[slot]) {
        s_port_buttons[slot] = 0;
        return;
    }
    if (s & JPF_JOY_UP)    b |= PSXB_UP;
    if (s & JPF_JOY_DOWN)  b |= PSXB_DOWN;
    if (s & JPF_JOY_LEFT)  b |= PSXB_LEFT;
    if (s & JPF_JOY_RIGHT) b |= PSXB_RIGHT;
    if (s & JPF_BUTTON_RED)  b |= PSXB_CROSS;
    if (s & JPF_BUTTON_BLUE) b |= PSXB_CIRCLE;
    if (type == JP_TYPE_GAMECTLR) {
        if (s & JPF_BUTTON_GREEN)   b |= PSXB_SQUARE;
        if (s & JPF_BUTTON_YELLOW)  b |= PSXB_TRIANGLE;
        if (s & JPF_BUTTON_REVERSE) b |= PSXB_L1;
        if (s & JPF_BUTTON_FORWARD) b |= PSXB_R1;
        if (s & JPF_BUTTON_PLAY)    b |= PSXB_START;
        /* No SELECT on a CD32 pad: both shoulders with PLAY give it. */
        if ((b & (PSXB_L1 | PSXB_R1 | PSXB_START)) == (PSXB_L1 | PSXB_R1 | PSXB_START))
            b = (b & ~(PSXB_L1 | PSXB_R1 | PSXB_START)) | PSXB_SELECT;
    }
    s_port_buttons[slot] = b;
}

/* Whether a message waits at the port, read from the PPC without a trip to
 * the 68k: an empty Exec list's head points at its own tail node. The 68k
 * (Intuition) writes the list, so the PPC may see it late; the caller also
 * drains the port every few frames regardless. */
static int port_has_message(struct MsgPort *port)
{
    volatile struct List *l = &port->mp_MsgList;
    return l->lh_Head != (struct Node *)&l->lh_Tail;
}

unsigned wos_input_poll(void *window)
{
    struct Window *win = (struct Window *)window;
    static unsigned frame;
    unsigned act = 0;

    /* Each OS call is a 68k trip (~2.5 ms for these two): the signals are
     * looked at once a second, the window's port only when it has mail. */
    if (++frame % 50 == 0) {
        ULONG sigs = SetSignal(0, 0);
        if (sigs & SIGBREAKF_CTRL_C) {
            SetSignal(0, SIGBREAKF_CTRL_C);
            act |= WOS_ACT_QUIT;
        }
    }
    {
        int i;
        for (i = 0; i < 128; i++)
            if (s_keylatch[i])
                s_keylatch[i]--;
    }
    if (frame % 1500 == 0)
        wos_log("[input] drains: %u on mail seen, %u by timer (%u of them with mail)",
                s_peek_drains, s_timer_drains, s_timer_mail);
    if (win && win->UserPort) {
        struct IntuiMessage *m;
        int peek = port_has_message(win->UserPort), got = 0;
        if (!peek && frame % 50 != 0)   /* the peek has missed nothing so far */
            goto ports;
        if (peek) s_peek_drains++; else s_timer_drains++;
        while ((m = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL) {
            got = 1;
            ULONG cls  = m->Class;
            UWORD code = m->Code;
            UWORD qual = m->Qualifier;
            ReplyMsg((struct Message *)m);
            if (cls == IDCMP_RAWKEY)
                act |= handle_key(code, qual);
            else if (cls == IDCMP_CLOSEWINDOW)
                act |= WOS_ACT_QUIT;
            else if (cls == IDCMP_INACTIVEWINDOW)
                memset(s_keydown, 0, sizeof(s_keydown));
        }
        if (got && !peek)
            s_timer_mail++;
    }
ports:

    if (s_joyports) {
        static unsigned turn;
        if (++turn & 1) read_port(1, 0);
        else            read_port(0, 1);
    }
    return act;
}

void wos_input_read_pad(int pad, WosPadState *st)
{
    uint16_t held;

    if (pad == 0) {
        held = keyboard_buttons() | s_port_buttons[0];
        st->connected = 1;
    } else {
        held = s_port_buttons[1];
        st->connected = s_port_live[1];
    }
    st->buttons = (uint16_t)~held;
    st->lx = st->ly = st->rx = st->ry = 128;
}
