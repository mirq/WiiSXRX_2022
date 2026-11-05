/*
 * WiiStationGUI -- a 68k launcher for WiiStation (WarpOS).
 *
 * A GadTools window on the Workbench: pick a disc image (ASL requester or
 * the recent list), set the emulator's options, add ENV: variables, Start.
 * The launcher writes T:WiiStationGo and runs it:
 *
 *   CD <WiiStation drawer>
 *   SetEnv <extra variables>             (removed again after the run)
 *   Execute WiiStation-R200 "<image>" Core=2 gpuPlugin=2 ...
 *
 * so the MiniGL environment stays in one place, the WiiStation-R200 script.
 * Settings and the recent list are kept in PROGDIR:WiiStationGUI.prefs.
 * The launcher waits for WiiStation to quit and then comes back.
 *
 * Build: m68k-amigaos-gcc -noixemul -Os (see Makefile in this directory).
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <graphics/text.h>
#include <workbench/startup.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include <proto/graphics.h>
#include <clib/alib_protos.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char version[] = "$VER: WiiStationGUI 1.0 (05.11.2026)";

struct Library *GadToolsBase;
struct Library *AslBase;

#define PREFS_FILE  "PROGDIR:WiiStationGUI.prefs"
#define GO_SCRIPT   "T:WiiStationGo"
#define MAX_RECENT  10
#define PATHLEN     512
#define EXTRALEN    512
#define VALLEN      64

#include "wsx_vars.h"

/* ---- Settings ---------------------------------------------------------- */

static struct {
    char image[PATHLEN];
    char drawer[PATHLEN];
    char biosfile[128];
    char extra[EXTRALEN];
    LONG core;      /* 0 dynarec, 1 interpreter */
    LONG gpu;       /* 0 Radeon, 1 P.E.Op.S., 2 gpulib */
    LONG pad;       /* 0 digital, 1 analog */
    LONG bios;      /* 0 file, 1 HLE */
    LONG sound;     /* 0 44100, 1 22050, 2 off */
    LONG dither;    /* 0 off, 1 default, 2 always */
    LONG interp;    /* 0 simple, 1 gaussian */
    LONG limit, bilinear, bootbios, ntsc, fps, window, mc1, mc2, vsync;
    char recent[MAX_RECENT][PATHLEN];
    int  nrecent;
    char vals[NVARS][VALLEN];    /* Variables window: "" = not set */
} S;

static void defaults(void)
{
    memset(&S, 0, sizeof(S));
    strcpy(S.biosfile, "SCPH1001.BIN");
    S.dither = 1;
    S.limit = S.bilinear = S.mc1 = S.mc2 = 1;
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
    return s;
}

static void copystr(char *d, const char *s, int n)
{
    strncpy(d, s, n - 1);
    d[n - 1] = 0;
}

static void load_prefs(void)
{
    char line[PATHLEN + 32];
    BPTR f = Open(PREFS_FILE, MODE_OLDFILE);
    if (!f)
        return;
    while (FGets(f, line, sizeof(line))) {
        char *k = trim(line), *v = strchr(k, '=');
        LONG n;
        if (!v || *k == '#' || *k == ';')
            continue;
        *v++ = 0;
        k = trim(k);
        v = trim(v);
        n = atol(v);
        if      (!stricmp(k, "Image"))    copystr(S.image, v, PATHLEN);
        else if (!stricmp(k, "Drawer"))   copystr(S.drawer, v, PATHLEN);
        else if (!stricmp(k, "BiosFile")) copystr(S.biosfile, v, sizeof(S.biosfile));
        else if (!stricmp(k, "Extra"))    copystr(S.extra, v, EXTRALEN);
        else if (!stricmp(k, "Core"))     S.core = n & 1;
        else if (!stricmp(k, "Gpu"))      S.gpu = n >= 0 && n <= 2 ? n : 0;
        else if (!stricmp(k, "Pad"))      S.pad = n & 1;
        else if (!stricmp(k, "Bios"))     S.bios = n & 1;
        else if (!stricmp(k, "Sound"))    S.sound = n >= 0 && n <= 2 ? n : 0;
        else if (!stricmp(k, "Dither"))   S.dither = n >= 0 && n <= 2 ? n : 1;
        else if (!stricmp(k, "Interp"))   S.interp = n & 1;
        else if (!stricmp(k, "Limit"))    S.limit = n != 0;
        else if (!stricmp(k, "Bilinear")) S.bilinear = n != 0;
        else if (!stricmp(k, "BootBios")) S.bootbios = n != 0;
        else if (!stricmp(k, "NTSC"))     S.ntsc = n != 0;
        else if (!stricmp(k, "FPS"))      S.fps = n != 0;
        else if (!stricmp(k, "Window"))   S.window = n != 0;
        else if (!stricmp(k, "VSync"))    S.vsync = n != 0;
        else if (!stricmp(k, "Memcard1")) S.mc1 = n != 0;
        else if (!stricmp(k, "Memcard2")) S.mc2 = n != 0;
        else if (!stricmp(k, "Var")) {
            char *e = strchr(v, '=');
            int i;
            if (e) {
                *e++ = 0;
                for (i = 0; i < NVARS; i++)
                    if (!stricmp(vars[i].name, trim(v)))
                        copystr(S.vals[i], trim(e), VALLEN);
            }
        }
        else if (!stricmp(k, "Recent") && *v && S.nrecent < MAX_RECENT)
            copystr(S.recent[S.nrecent++], v, PATHLEN);
    }
    Close(f);
}

static BOOL save_prefs(void)
{
    int i;
    BPTR f = Open(PREFS_FILE, MODE_NEWFILE);
    if (!f)
        return FALSE;
    FPrintf(f, "# WiiStationGUI settings\n");
    FPrintf(f, "Image=%s\nDrawer=%s\nBiosFile=%s\nExtra=%s\n",
            (LONG)S.image, (LONG)S.drawer, (LONG)S.biosfile, (LONG)S.extra);
    FPrintf(f, "Core=%ld\nGpu=%ld\nPad=%ld\nBios=%ld\nSound=%ld\nDither=%ld\nInterp=%ld\n",
            S.core, S.gpu, S.pad, S.bios, S.sound, S.dither, S.interp);
    FPrintf(f, "Limit=%ld\nBilinear=%ld\nBootBios=%ld\nNTSC=%ld\nFPS=%ld\nWindow=%ld\nVSync=%ld\n",
            S.limit, S.bilinear, S.bootbios, S.ntsc, S.fps, S.window, S.vsync);
    FPrintf(f, "Memcard1=%ld\nMemcard2=%ld\n", S.mc1, S.mc2);
    for (i = 0; i < NVARS; i++)
        if (S.vals[i][0])
            FPrintf(f, "Var=%s=%s\n", (LONG)vars[i].name, (LONG)S.vals[i]);
    for (i = 0; i < S.nrecent; i++)
        FPrintf(f, "Recent=%s\n", (LONG)S.recent[i]);
    Close(f);
    return TRUE;
}

static void add_recent(const char *path)
{
    int i, j;
    for (i = 0; i < S.nrecent; i++)
        if (!stricmp(S.recent[i], path))
            break;
    if (i == S.nrecent) {               /* new: drop the oldest when full */
        if (S.nrecent < MAX_RECENT)
            S.nrecent++;
        i = S.nrecent - 1;
    }
    for (j = i; j > 0; j--)
        strcpy(S.recent[j], S.recent[j - 1]);
    copystr(S.recent[0], path, PATHLEN);
}

/* ---- The window -------------------------------------------------------- */

enum {
    G_IMAGE, G_PICK, G_RECENT, G_FORGET,
    G_CORE, G_GPU, G_PAD, G_BIOS, G_BIOSFILE, G_SOUND, G_DITHER, G_INTERP,
    G_LIMIT, G_BILINEAR, G_BOOTBIOS, G_NTSC, G_FPS, G_WINDOW, G_VSYNC, G_MC1, G_MC2,
    G_EXTRA, G_VARS, G_START, G_SAVE, G_QUIT, G_COUNT
};

static const char *core_labels[]   = { "PPC dynarec", "Interpreter", NULL };
static const char *gpu_labels[]    = { "Radeon 9200 (MiniGL)", "Software (P.E.Op.S.)", "Software (gpulib)", NULL };
static const char *pad_labels[]    = { "Digital", "Analog (DualShock)", NULL };
static const char *bios_labels[]   = { "BIOS file", "HLE (no BIOS file)", NULL };
static const char *sound_labels[]  = { "On, 44100 Hz", "On, 22050 Hz (less 68k work)", "Off", NULL };
static const char *dither_labels[] = { "Off", "Game's choice", "Always", NULL };
static const char *interp_labels[] = { "Simple", "Gaussian", NULL };

static struct Screen   *scr;
static APTR             vi;
static struct Window   *win;
static struct Gadget   *glist;
static struct Gadget   *gad[G_COUNT];
static struct TextAttr  tattr;
static struct List      recent_list;
static struct Node      recent_nodes[MAX_RECENT];
static char             status_text[160] = "Pick a disc image and press Start.";
static WORD             status_y, status_x, status_w, hint_y;

static void build_recent_list(void)
{
    int i;
    NewList(&recent_list);
    for (i = 0; i < S.nrecent; i++) {
        recent_nodes[i].ln_Name = S.recent[i];
        AddTail(&recent_list, &recent_nodes[i]);
    }
}

static void refresh_recent(void)
{
    GT_SetGadgetAttrs(gad[G_RECENT], win, NULL, GTLV_Labels, ~0, TAG_DONE);
    build_recent_list();
    GT_SetGadgetAttrs(gad[G_RECENT], win, NULL, GTLV_Labels, (ULONG)&recent_list,
                      GTLV_Selected, ~0, TAG_DONE);
}

static void set_status(const char *msg)
{
    struct RastPort *rp;
    copystr(status_text, msg, sizeof(status_text));
    if (!win)
        return;
    rp = win->RPort;
    SetAPen(rp, 0);
    RectFill(rp, status_x, status_y, status_x + status_w - 1,
             status_y + rp->TxHeight + 1);
    SetAPen(rp, 1);
    SetBPen(rp, 0);
    Move(rp, status_x, status_y + rp->TxBaseline);
    {
        /* clip the text to the window */
        struct TextExtent te;
        ULONG n = TextFit(rp, (STRPTR)status_text, strlen(status_text), &te,
                          NULL, 1, status_w, rp->TxHeight + 1);
        Text(rp, (STRPTR)status_text, n);
    }
}

static WORD main_w;

/* Text into a box, word-wrapped over up to `lines` lines. */
static void draw_wrapped(struct RastPort *rp, WORD x, WORD y, WORD w, int lines,
                         const char *t, BOOL clear)
{
    WORD lh = rp->TxHeight + 1;
    int l;
    if (clear) {
        SetAPen(rp, 0);
        RectFill(rp, x, y, x + w - 1, y + lines * lh - 1);
    }
    SetAPen(rp, 1);
    SetBPen(rp, 0);
    for (l = 0; l < lines && *t; l++) {
        struct TextExtent te;
        ULONG len = strlen(t), n;
        const char *nl = strchr(t, '\n');
        if (nl && (ULONG)(nl - t) < len)
            len = nl - t;
        n = TextFit(rp, (STRPTR)t, len, &te, NULL, 1, w, lh);
        if (n < len) {                      /* break at the last space */
            ULONG k = n;
            while (k > 0 && t[k] != ' ')
                k--;
            if (k > 0)
                n = k;
        }
        Move(rp, x, y + l * lh + rp->TxBaseline);
        Text(rp, (STRPTR)t, n);
        t += n;
        if (*t == '\n')
            t++;
        while (*t == ' ')
            t++;
    }
}

/* The line under Extra ENV: what the Variables window sets for the run. */
static void draw_hint(void)
{
    char buf[400];
    int i, n = 0;
    buf[0] = 0;
    for (i = 0; i < NVARS && strlen(buf) < sizeof(buf) - 90; i++)
        if (S.vals[i][0]) {
            if (!n++)
                strcpy(buf, "Variables: ");
            else
                strcat(buf, "  ");
            strcat(buf, vars[i].name);
            strcat(buf, "=");
            strcat(buf, S.vals[i]);
        }
    if (!n)
        strcpy(buf, "Extra ENV: NAME=value, this run only. Variables... lists all, with help.");
    draw_wrapped(win->RPort, status_x, hint_y, status_w, 1, buf, TRUE);
}

static WORD textw(struct RastPort *rp, const char *s)
{
    return TextLength(rp, (STRPTR)s, strlen(s));
}

static BOOL open_window(void)
{
    struct NewGadget ng;
    struct Gadget *g;
    struct RastPort rp;
    struct TextFont *font;
    WORD fh, row, lw, x0, y, w, colw, bw, i, cw, ih;
    static const char *labels[] = {
        "CPU", "Renderer", "Pads", "BIOS", "BIOS file", "Sound",
        "Dithering", "Sound filter", "Extra ENV:", NULL
    };
    static const char *checks[] = {
        "Frame limiter", "Bilinear filter", "Boot through BIOS", "Force NTSC",
        "Show FPS", "Workbench window", "Wait for vsync", "Memory card 1", "Memory card 2"
    };
    static const UWORD check_ids[] = {
        G_LIMIT, G_BILINEAR, G_BOOTBIOS, G_NTSC, G_FPS, G_WINDOW, G_VSYNC, G_MC1, G_MC2
    };
    static const LONG *check_vals[9];

    if (!(scr = LockPubScreen(NULL)))
        return FALSE;
    if (!(vi = GetVisualInfo(scr, TAG_DONE)))
        return FALSE;

    tattr = *scr->Font;
    InitRastPort(&rp);
    if ((font = OpenFont(&tattr)) != NULL)
        SetFont(&rp, font);
    fh = tattr.ta_YSize;
    row = fh + 6;

    lw = 0;
    for (i = 0; labels[i]; i++)
        if (textw(&rp, labels[i]) > lw)
            lw = textw(&rp, labels[i]);
    lw += 16;
    cw = 0;
    for (i = 0; i < 9; i++)
        if (textw(&rp, checks[i]) > cw)
            cw = textw(&rp, checks[i]);
    colw = cw + 26 + 16;                    /* checkbox + gap + label */
    w = textw(&rp, "Software (P.E.Op.S.)") + 48;
    if (w < textw(&rp, "On, 22050 Hz (less 68k work)") + 40)
        w = textw(&rp, "On, 22050 Hz (less 68k work)") + 40;
    w = lw + w + 16 + 3 * colw / 2;         /* settings column + checkbox column */
    if (w < 72 * textw(&rp, "n"))
        w = 72 * textw(&rp, "n");
    bw = textw(&rp, "Save settings") + 24;
    if (font)
        CloseFont(font);

    x0 = scr->WBorLeft + 10;
    y = scr->WBorTop + scr->Font->ta_YSize + 1 + 8;

    if (!(g = CreateContext(&glist)))
        return FALSE;
    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr = &tattr;
    ng.ng_VisualInfo = vi;

    /* Image: string + Pick */
    ng.ng_LeftEdge = x0 + lw;
    ng.ng_TopEdge = y;
    ng.ng_Width = w - lw - bw - 8;
    ng.ng_Height = row;
    ng.ng_GadgetText = "Disc image";
    ng.ng_GadgetID = G_IMAGE;
    ng.ng_Flags = PLACETEXT_LEFT;
    g = gad[G_IMAGE] = CreateGadget(STRING_KIND, g, &ng,
                                    GTST_String, (ULONG)S.image, GTST_MaxChars, PATHLEN - 1,
                                    GT_Underscore, '_', TAG_DONE);
    ng.ng_LeftEdge = x0 + w - bw;
    ng.ng_Width = bw;
    ng.ng_GadgetText = "_Pick...";
    ng.ng_GadgetID = G_PICK;
    ng.ng_Flags = PLACETEXT_IN;
    g = gad[G_PICK] = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_DONE);
    y += row + 4;

    /* Recent list + Forget */
    ih = fh + 1;
    ng.ng_LeftEdge = x0 + lw;
    ng.ng_TopEdge = y;
    ng.ng_Width = w - lw - bw - 8;
    ng.ng_Height = 6 * ih + 4;
    ng.ng_GadgetText = "Recent";
    ng.ng_GadgetID = G_RECENT;
    ng.ng_Flags = PLACETEXT_LEFT;
    build_recent_list();
    g = gad[G_RECENT] = CreateGadget(LISTVIEW_KIND, g, &ng,
                                     GTLV_Labels, (ULONG)&recent_list,
                                     GTLV_ShowSelected, 0, TAG_DONE);
    ng.ng_LeftEdge = x0 + w - bw;
    ng.ng_Width = bw;
    ng.ng_Height = row;
    ng.ng_GadgetText = "_Forget";
    ng.ng_GadgetID = G_FORGET;
    ng.ng_Flags = PLACETEXT_IN;
    g = gad[G_FORGET] = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_DONE);
    y += 6 * ih + 4 + 10;

    /* Settings column (cycles) and checkbox column */
    {
        WORD cyw = w - lw - colw - 16, ys = y;
        struct { UWORD id; const char *label; const char **labels; LONG *val; } cy[] = {
            { G_CORE,   "CPU",          core_labels,   &S.core },
            { G_GPU,    "Renderer",     gpu_labels,    &S.gpu },
            { G_PAD,    "Pads",         pad_labels,    &S.pad },
            { G_BIOS,   "BIOS",         bios_labels,   &S.bios },
            { 0, NULL, NULL, NULL },                    /* BIOS file string */
            { G_SOUND,  "Sound",        sound_labels,  &S.sound },
            { G_INTERP, "Sound filter", interp_labels, &S.interp },
            { G_DITHER, "Dithering",    dither_labels, &S.dither },
        };
        for (i = 0; i < (WORD)(sizeof(cy) / sizeof(cy[0])); i++) {
            ng.ng_LeftEdge = x0 + lw;
            ng.ng_TopEdge = y;
            ng.ng_Width = cyw;
            ng.ng_Height = row;
            ng.ng_Flags = PLACETEXT_LEFT;
            if (!cy[i].label) {
                ng.ng_GadgetText = "BIOS file";
                ng.ng_GadgetID = G_BIOSFILE;
                g = gad[G_BIOSFILE] = CreateGadget(STRING_KIND, g, &ng,
                                                   GTST_String, (ULONG)S.biosfile,
                                                   GTST_MaxChars, sizeof(S.biosfile) - 1,
                                                   GA_Disabled, S.bios, TAG_DONE);
            } else {
                ng.ng_GadgetText = (UBYTE *)cy[i].label;
                ng.ng_GadgetID = cy[i].id;
                g = gad[cy[i].id] = CreateGadget(CYCLE_KIND, g, &ng,
                                                 GTCY_Labels, (ULONG)cy[i].labels,
                                                 GTCY_Active, *cy[i].val, TAG_DONE);
            }
            y += row + 3;
        }

        check_vals[0] = &S.limit;   check_vals[1] = &S.bilinear;
        check_vals[2] = &S.bootbios; check_vals[3] = &S.ntsc;
        check_vals[4] = &S.fps;     check_vals[5] = &S.window;
        check_vals[6] = &S.vsync;   check_vals[7] = &S.mc1;
        check_vals[8] = &S.mc2;
        for (i = 0; i < 9; i++) {
            ng.ng_LeftEdge = x0 + w - colw + 8;
            ng.ng_TopEdge = ys + i * (row + 3) * 8 / 9;
            ng.ng_Width = 26;
            ng.ng_Height = fh + 3;
            ng.ng_GadgetText = (UBYTE *)checks[i];
            ng.ng_GadgetID = check_ids[i];
            ng.ng_Flags = PLACETEXT_RIGHT;
            g = gad[check_ids[i]] = CreateGadget(CHECKBOX_KIND, g, &ng,
                                                 GTCB_Checked, *check_vals[i],
                                                 GTCB_Scaled, TRUE, TAG_DONE);
        }
    }
    y += 6;

    /* Extra ENV: variables */
    ng.ng_LeftEdge = x0 + lw;
    ng.ng_TopEdge = y;
    ng.ng_Width = w - lw - bw - 8;
    ng.ng_Height = row;
    ng.ng_GadgetText = "Extra ENV:";
    ng.ng_GadgetID = G_EXTRA;
    ng.ng_Flags = PLACETEXT_LEFT;
    g = gad[G_EXTRA] = CreateGadget(STRING_KIND, g, &ng,
                                    GTST_String, (ULONG)S.extra,
                                    GTST_MaxChars, EXTRALEN - 1, TAG_DONE);
    ng.ng_LeftEdge = x0 + w - bw;
    ng.ng_Width = bw;
    ng.ng_GadgetText = "Va_riables...";
    ng.ng_GadgetID = G_VARS;
    ng.ng_Flags = PLACETEXT_IN;
    g = gad[G_VARS] = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_DONE);
    y += row + 4;
    hint_y = y;             /* the hint line, then the status line */
    y += fh + 4;
    status_x = x0 + lw;
    status_y = y;
    status_w = w - lw;
    y += fh + 10;

    /* Buttons */
    {
        WORD b3 = (w - 2 * 12) / 3;
        static const char *bl[] = { "_Start", "Sa_ve settings", "_Quit" };
        for (i = 0; i < 3; i++) {
            ng.ng_LeftEdge = x0 + i * (b3 + 12);
            ng.ng_TopEdge = y;
            ng.ng_Width = b3;
            ng.ng_Height = row + 2;
            ng.ng_GadgetText = (UBYTE *)bl[i];
            ng.ng_GadgetID = G_START + i;
            ng.ng_Flags = PLACETEXT_IN;
            g = gad[G_START + i] = CreateGadget(BUTTON_KIND, g, &ng,
                                                GT_Underscore, '_', TAG_DONE);
        }
    }
    y += row + 2 + 8;

    if (!g)
        return FALSE;

    win = OpenWindowTags(NULL,
        WA_Title, (ULONG)"WiiStation launcher",
        WA_ScreenTitle, (ULONG)(version + 6),
        WA_PubScreen, (ULONG)scr,
        WA_Left, (scr->Width - (x0 + w + 10 + scr->WBorRight)) / 2,
        WA_Top, scr->BarHeight + 4,
        WA_InnerWidth, x0 - scr->WBorLeft + w + 10,
        WA_InnerHeight, y - (scr->WBorTop + scr->Font->ta_YSize + 1),
        WA_Gadgets, (ULONG)glist,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
        WA_Activate, TRUE, WA_SmartRefresh, TRUE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY |
                  BUTTONIDCMP | STRINGIDCMP | CYCLEIDCMP | CHECKBOXIDCMP | LISTVIEWIDCMP,
        TAG_DONE);
    if (!win)
        return FALSE;
    SetFont(win->RPort, OpenFont(&tattr));
    GT_RefreshWindow(win, NULL);
    main_w = w;
    draw_hint();
    set_status(status_text);
    return TRUE;
}

static void close_window(void)
{
    if (win) {
        struct TextFont *f = win->RPort->Font;
        CloseWindow(win);
        if (f)
            CloseFont(f);
        win = NULL;
    }
    FreeGadgets(glist);
    glist = NULL;
    if (vi)
        FreeVisualInfo(vi);
    vi = NULL;
    if (scr)
        UnlockPubScreen(NULL, scr);
    scr = NULL;
}

static void read_strings(void)
{
    STRPTR s;
    GT_GetGadgetAttrs(gad[G_IMAGE], win, NULL, GTST_String, (ULONG)&s, TAG_DONE);
    copystr(S.image, (char *)s, PATHLEN);
    GT_GetGadgetAttrs(gad[G_BIOSFILE], win, NULL, GTST_String, (ULONG)&s, TAG_DONE);
    copystr(S.biosfile, (char *)s, sizeof(S.biosfile));
    GT_GetGadgetAttrs(gad[G_EXTRA], win, NULL, GTST_String, (ULONG)&s, TAG_DONE);
    copystr(S.extra, (char *)s, EXTRALEN);
}

static void set_image(const char *path)
{
    copystr(S.image, path, PATHLEN);
    GT_SetGadgetAttrs(gad[G_IMAGE], win, NULL, GTST_String, (ULONG)S.image, TAG_DONE);
}

/* ---- ASL requester ----------------------------------------------------- */

static void pick_image(void)
{
    struct FileRequester *fr;
    char drawer[PATHLEN], file[128];
    char *p;

    read_strings();
    drawer[0] = file[0] = 0;
    if (S.image[0]) {                       /* start where the current image is */
        copystr(drawer, S.image, PATHLEN);
        p = (char *)PathPart((STRPTR)drawer);
        copystr(file, (char *)FilePart((STRPTR)S.image), sizeof(file));
        *p = 0;
    } else if (S.drawer[0])
        copystr(drawer, S.drawer, PATHLEN);

    fr = AllocAslRequestTags(ASL_FileRequest,
        ASLFR_Window, (ULONG)win,
        ASLFR_SleepWindow, TRUE,
        ASLFR_TitleText, (ULONG)"Choose a PlayStation disc image",
        ASLFR_InitialDrawer, (ULONG)drawer,
        ASLFR_InitialFile, (ULONG)file,
        ASLFR_InitialPattern, (ULONG)"#?.(cue|bin|iso|img|ccd|mds|mdf|toc|pbp|cbn|chd|exe)",
        ASLFR_DoPatterns, TRUE,
        ASLFR_RejectIcons, TRUE,
        TAG_DONE);
    if (!fr) {
        set_status("Could not open the ASL requester.");
        return;
    }
    if (AslRequest(fr, NULL) && fr->fr_File && fr->fr_File[0]) {
        char path[PATHLEN];
        copystr(path, (char *)fr->fr_Drawer, PATHLEN);
        copystr(S.drawer, (char *)fr->fr_Drawer, PATHLEN);
        AddPart((STRPTR)path, fr->fr_File, PATHLEN);
        set_image(path);
        set_status("Press Start to run it.");
    }
    FreeAslRequest(fr);
}

/* ---- Running WiiStation ------------------------------------------------ */

/* Is name a plain ENV: variable name (letters, digits, '_')? */
static BOOL good_var(const char *s)
{
    if (!*s)
        return FALSE;
    for (; *s; s++)
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
              (*s >= '0' && *s <= '9') || *s == '_'))
            return FALSE;
    return TRUE;
}

/* The settings WiiStation takes as Key=value arguments. */
static void put_args(BPTR f)
{
    static const LONG gpu_val[] = { 2, 0, 1 };
    FPrintf(f, " Core=%ld gpuPlugin=%ld ControllerType=%ld",
            S.core ? 1L : 2L, gpu_val[S.gpu], S.pad);
    FPrintf(f, " BiosDevice=%ld", S.bios ? 0L : 1L);
    if (!S.bios && S.biosfile[0] && !strchr(S.biosfile, ' ') && !strchr(S.biosfile, '"'))
        FPrintf(f, " BiosFile=%s", (LONG)S.biosfile);
    FPrintf(f, " Audio=%ld Interpolation=%ld Dithering=%ld",
            S.sound == 2 ? 0L : 1L, S.interp + 1, S.dither);
    FPrintf(f, " LimitFrames=%ld BilinearFilter=%ld BootThruBios=%ld ForceNTSC=%ld",
            S.limit, S.bilinear, S.bootbios, S.ntsc);
    FPrintf(f, " Memcard0=%ld Memcard1=%ld", S.mc1, S.mc2);
}

/* ENV: variables the launcher owns: set for the run as chosen, or removed. */
static void own_var(const char *name, BOOL on, const char *val)
{
    if (on)
        SetVar((STRPTR)name, (STRPTR)val, -1, GVF_GLOBAL_ONLY);
    else
        DeleteVar((STRPTR)name, GVF_GLOBAL_ONLY);
}

/* One-shot diagnosis variables that stay in ENV: and change the next run
 * (WSX_QUIT quits it at frame 50, WSX_LOADSTATE loads a state): removed
 * before every run unless they are given in Extra ENV:. */
static const char *oneshot[] = {
    "WSX_QUIT", "WSX_LOADSTATE", "WSX_AUTOFRAMES", "WSX_SAVESTATE",
    "WSX_SHOT", "WSX_SHOTAT", "WSX_PRIMPROBE", NULL
};

static BOOL in_extra(const char *name)
{
    char buf[EXTRALEN], *t;
    copystr(buf, S.extra, EXTRALEN);
    for (t = strtok(buf, " \t"); t; t = strtok(NULL, " \t")) {
        char *eq = strchr(t, '=');
        if (eq)
            *eq = 0;
        if (!stricmp(t, name))
            return TRUE;
    }
    return FALSE;
}

/* Extra variables: NAME=value or NAME (= 1); set, or removed again. */
static void extra_vars(BOOL set)
{
    char buf[EXTRALEN], *t;
    copystr(buf, S.extra, EXTRALEN);
    for (t = strtok(buf, " \t"); t; t = strtok(NULL, " \t")) {
        char *eq = strchr(t, '=');
        const char *val = "1";
        if (eq) {
            *eq = 0;
            val = eq + 1;
        }
        if (!good_var(t))
            continue;
        if (set && *val)
            SetVar((STRPTR)t, (STRPTR)val, -1, GVF_GLOBAL_ONLY);
        else if (!set)
            DeleteVar((STRPTR)t, GVF_GLOBAL_ONLY);
    }
}

static int var_index(const char *name)
{
    int i;
    for (i = 0; i < NVARS; i++)
        if (!stricmp(vars[i].name, name))
            return i;
    return -1;
}

/* What ENV: held before the run, for the variables the window sets. */
static char old_vals[NVARS][VALLEN];
static BYTE had_old[NVARS];

static void table_vars(BOOL set)
{
    int i;
    for (i = 0; i < NVARS; i++) {
        if (!S.vals[i][0])
            continue;
        if (set) {
            had_old[i] = GetVar((STRPTR)vars[i].name, (STRPTR)old_vals[i], VALLEN,
                                GVF_GLOBAL_ONLY) >= 0;
            SetVar((STRPTR)vars[i].name, (STRPTR)S.vals[i], -1, GVF_GLOBAL_ONLY);
        } else if (had_old[i])
            SetVar((STRPTR)vars[i].name, (STRPTR)old_vals[i], -1, GVF_GLOBAL_ONLY);
        else
            DeleteVar((STRPTR)vars[i].name, GVF_GLOBAL_ONLY);
    }
}

static void env_before(void)
{
    int i;
    for (i = 0; oneshot[i]; i++) {
        int k = var_index(oneshot[i]);
        if (!in_extra(oneshot[i]) && (k < 0 || !S.vals[k][0]))
            DeleteVar((STRPTR)oneshot[i], GVF_GLOBAL_ONLY);
    }
    own_var("WSX_WINDOW", S.window, "1");
    own_var("WSX_SHOWFPS", S.fps, "1");
    own_var("WSX_VSYNC", S.vsync, "1");
    own_var("WSX_AHI_MIXRATE", S.sound == 1, "22050");
    table_vars(TRUE);
    extra_vars(TRUE);
}

static void env_after(void)
{
    extra_vars(FALSE);
    table_vars(FALSE);
    DeleteVar((STRPTR)"WSX_QUIT", GVF_GLOBAL_ONLY);
}

static BOOL write_go_script(const char *progdir)
{
    BPTR f = Open(GO_SCRIPT, MODE_NEWFILE);
    if (!f)
        return FALSE;
    FPrintf(f, "; Written by WiiStationGUI: runs WiiStation with the launcher's settings.\n");
    FPrintf(f, "FailAt 21\nCD \"%s\"\n", (LONG)progdir);
    FPrintf(f, "Execute WiiStation-R200 \"%s\"", (LONG)S.image);
    put_args(f);
    FPrintf(f, "\n");
    Close(f);
    return TRUE;
}

static BOOL file_exists(const char *path)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (l) {
        UnLock(l);
        return TRUE;
    }
    return FALSE;
}

static void run_game(void)
{
    char progdir[PATHLEN], check[PATHLEN], cmd[64];
    BPTR pd, out, in;
    LONG rc;
    char *p;

    read_strings();
    for (p = S.image; *p; p++)              /* the script quotes the path */
        if (*p == '"') {
            set_status("The image path must not contain quotes.");
            return;
        }
    if (!S.image[0]) {
        pick_image();
        if (!S.image[0])
            return;
    }
    if (strchr(S.image, '=')) {
        set_status("WiiStation cannot take a path with '=' in it.");
        return;
    }
    if (!file_exists(S.image)) {
        set_status("The image file was not found.");
        DisplayBeep(scr);
        return;
    }
    /* Name the image fully: the script runs from the WiiStation drawer. */
    {
        BPTR l = Lock((STRPTR)S.image, ACCESS_READ);
        if (l) {
            if (NameFromLock(l, (STRPTR)check, PATHLEN))
                set_image(check);
            UnLock(l);
        }
    }

    pd = GetProgramDir();
    if (!pd || !NameFromLock(pd, (STRPTR)progdir, PATHLEN)) {
        set_status("Cannot find the launcher's drawer.");
        return;
    }
    copystr(check, progdir, PATHLEN);
    AddPart((STRPTR)check, (STRPTR)"WiiStation", PATHLEN);
    if (!file_exists(check)) {
        set_status("WiiStation is not in the launcher's drawer.");
        return;
    }
    copystr(check, progdir, PATHLEN);
    AddPart((STRPTR)check, (STRPTR)"WiiStation-R200", PATHLEN);
    if (!file_exists(check)) {
        set_status("WiiStation-R200 (the launch script) is missing.");
        return;
    }

    add_recent(S.image);
    refresh_recent();
    save_prefs();
    if (!write_go_script(progdir)) {
        set_status("Cannot write T:WiiStationGo.");
        return;
    }

    set_status("WiiStation is running...");
    SetWindowPointer(win, WA_BusyPointer, TRUE, TAG_DONE);

    env_before();
    out = Open("NIL:", MODE_NEWFILE);
    in = Open("NIL:", MODE_OLDFILE);
    strcpy(cmd, "Execute " GO_SCRIPT);
    rc = SystemTags((STRPTR)cmd,
                    SYS_Input, (ULONG)in,
                    SYS_Output, (ULONG)out,
                    SYS_UserShell, TRUE,
                    NP_StackSize, 65536,
                    TAG_DONE);
    if (rc == -1) {                         /* the command never started */
        if (in) Close(in);
        if (out) Close(out);
    }
    env_after();
    SetWindowPointer(win, WA_Pointer, 0, TAG_DONE);
    if (rc == 0)
        set_status("WiiStation finished. The log is wiistation.log in its drawer.");
    else {
        char msg[96];
        sprintf(msg, "WiiStation returned %ld: see wiistation.log in its drawer.", (long)rc);
        set_status(msg);
    }
    /* back to the front */
    ScreenToFront(win->WScreen);
    WindowToFront(win);
    ActivateWindow(win);
}

/* ---- The Variables window ---------------------------------------------- */

enum { V_FILTER = 100, V_LIST, V_VALUE, V_SET, V_DEFAULT, V_CLEARALL, V_CLOSE };

static char        var_lines[NVARS][VALLEN + 48];
static struct Node var_nodes[NVARS];
static struct List var_list;
static WORD        var_map[NVARS];
static int         var_n;
static LONG        var_filter;              /* 0 all, 1.. a group, NGROUPS+1 set */
static const char *filter_labels[NGROUPS + 3];

static void env_now(const char *name, char *buf, int n)
{
    if (GetVar((STRPTR)name, (STRPTR)buf, n, GVF_GLOBAL_ONLY) < 0)
        buf[0] = 0;
}

static void build_var_list(void)
{
    int i;
    NewList(&var_list);
    var_n = 0;
    for (i = 0; i < NVARS; i++) {
        char env[VALLEN];
        char *l = var_lines[var_n];
        env_now(vars[i].name, env, VALLEN);
        if (var_filter >= 1 && var_filter <= NGROUPS && vars[i].group != var_filter - 1)
            continue;
        if (var_filter == NGROUPS + 1 && !S.vals[i][0] && !env[0])
            continue;
        if (S.vals[i][0])
            sprintf(l, "%-19s= %s", vars[i].name, S.vals[i]);
        else if (env[0])
            sprintf(l, "%-19s  (in ENV: %s)", vars[i].name, env);
        else
            sprintf(l, "%-19s  (default %s)", vars[i].name, vars[i].def);
        var_nodes[var_n].ln_Name = l;
        AddTail(&var_list, &var_nodes[var_n]);
        var_map[var_n++] = i;
    }
}

static void vars_window(void)
{
    struct Gadget *vlist = NULL, *g, *vg[8];
    struct NewGadget ng;
    struct Window *vw;
    WORD fh = tattr.ta_YSize, row = fh + 6, x0, y, w = main_w, bw, i;
    WORD desc_y, desc_lines = 5, rows = 14;
    LONG sel = -1, sel_row = -1;
    BOOL done = FALSE;
    char info[600];

    filter_labels[0] = "All";
    for (i = 0; i < NGROUPS; i++)
        filter_labels[i + 1] = var_groups[i];
    filter_labels[NGROUPS + 1] = "Set now (launcher or ENV:)";
    filter_labels[NGROUPS + 2] = NULL;

    x0 = scr->WBorLeft + 10;
    y = scr->WBorTop + scr->Font->ta_YSize + 1 + 8;
    bw = (w - 3 * 8) / 4;
    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr = &tattr;
    ng.ng_VisualInfo = vi;
    if (!(g = CreateContext(&vlist)))
        return;

    ng.ng_LeftEdge = x0 + 60;
    ng.ng_TopEdge = y;
    ng.ng_Width = w - 60;
    ng.ng_Height = row;
    ng.ng_GadgetText = "Show";
    ng.ng_GadgetID = V_FILTER;
    ng.ng_Flags = PLACETEXT_LEFT;
    g = vg[0] = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)filter_labels,
                             GTCY_Active, var_filter, TAG_DONE);
    y += row + 6;

    build_var_list();
    ng.ng_LeftEdge = x0;
    ng.ng_TopEdge = y;
    ng.ng_Width = w;
    ng.ng_Height = rows * (fh + 1) + 4;
    ng.ng_GadgetText = NULL;
    ng.ng_GadgetID = V_LIST;
    g = vg[1] = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&var_list,
                             GTLV_ShowSelected, 0, TAG_DONE);
    y += rows * (fh + 1) + 4 + 6;
    desc_y = y;
    y += desc_lines * (fh + 1) + 8;

    ng.ng_LeftEdge = x0 + 60;
    ng.ng_TopEdge = y;
    ng.ng_Width = w - 60;
    ng.ng_Height = row;
    ng.ng_GadgetText = "Value";
    ng.ng_GadgetID = V_VALUE;
    ng.ng_Flags = PLACETEXT_LEFT;
    g = vg[2] = CreateGadget(STRING_KIND, g, &ng, GTST_MaxChars, VALLEN - 1,
                             GA_Disabled, TRUE, TAG_DONE);
    y += row + 8;
    {
        static const char *bl[] = { "_Set", "_Default", "Clear _all", "_Close" };
        for (i = 0; i < 4; i++) {
            ng.ng_LeftEdge = x0 + i * (bw + 8);
            ng.ng_TopEdge = y;
            ng.ng_Width = bw;
            ng.ng_Height = row + 2;
            ng.ng_GadgetText = (UBYTE *)bl[i];
            ng.ng_GadgetID = V_SET + i;
            ng.ng_Flags = PLACETEXT_IN;
            g = vg[3 + i] = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_DONE);
        }
    }
    y += row + 2 + 8;
    if (!g) {
        FreeGadgets(vlist);
        return;
    }

    vw = OpenWindowTags(NULL,
        WA_Title, (ULONG)"WiiStation ENV: variables",
        WA_PubScreen, (ULONG)scr,
        WA_Left, win->LeftEdge + 20,
        WA_Top, scr->BarHeight + 2,
        WA_InnerWidth, x0 - scr->WBorLeft + w + 10,
        WA_InnerHeight, y - (scr->WBorTop + scr->Font->ta_YSize + 1),
        WA_Gadgets, (ULONG)vlist,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
        WA_Activate, TRUE, WA_SmartRefresh, TRUE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY |
                  BUTTONIDCMP | STRINGIDCMP | CYCLEIDCMP | LISTVIEWIDCMP,
        TAG_DONE);
    if (!vw) {
        FreeGadgets(vlist);
        set_status("Could not open the Variables window.");
        return;
    }
    SetFont(vw->RPort, OpenFont(&tattr));
    GT_RefreshWindow(vw, NULL);
    SetWindowPointer(win, WA_BusyPointer, TRUE, TAG_DONE);
#define SHOW_INFO(t) draw_wrapped(vw->RPort, x0, desc_y, w, desc_lines, (t), TRUE)
    SHOW_INFO("Pick a variable to read what it does. Values set here go into ENV: for "
              "the next run only (the old ENV: value comes back afterwards). Default "
              "also removes the variable from ENV:. The list is in WiiStation-Vars.txt too.");

    while (!done) {
        struct IntuiMessage *m;
        WaitPort(vw->UserPort);
        while (!done && (m = GT_GetIMsg(vw->UserPort)) != NULL) {
            ULONG cls = m->Class;
            UWORD code = m->Code;
            struct Gadget *gg = (struct Gadget *)m->IAddress;
            UWORD id = (cls == IDCMP_GADGETUP) ? gg->GadgetID : 0;
            GT_ReplyIMsg(m);
            if (cls == IDCMP_CLOSEWINDOW || (cls == IDCMP_VANILLAKEY && code == 27))
                done = TRUE;
            else if (cls == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(vw);
                GT_EndRefresh(vw, TRUE);
            } else if (cls == IDCMP_VANILLAKEY) {
                if (code == 'c' || code == 'C')
                    done = TRUE;
            }
            if (id == V_CLOSE)
                done = TRUE;
            else if (id == V_FILTER) {
                var_filter = code;
                GT_SetGadgetAttrs(vg[1], vw, NULL, GTLV_Labels, ~0, TAG_DONE);
                build_var_list();
                GT_SetGadgetAttrs(vg[1], vw, NULL, GTLV_Labels, (ULONG)&var_list,
                                  GTLV_Selected, ~0, GTLV_Top, 0, TAG_DONE);
                sel = sel_row = -1;
                GT_SetGadgetAttrs(vg[2], vw, NULL, GTST_String, (ULONG)"", GA_Disabled, TRUE, TAG_DONE);
            } else if (id == V_LIST && code < var_n) {
                char env[VALLEN];
                sel_row = code;
                sel = var_map[code];
                env_now(vars[sel].name, env, VALLEN);
                sprintf(info, "%s  [%s]   default: %s%s%s\n%s", vars[sel].name,
                        var_groups[vars[sel].group], vars[sel].def,
                        env[0] ? "   now in ENV: " : "", env, vars[sel].desc);
                SHOW_INFO(info);
                GT_SetGadgetAttrs(vg[2], vw, NULL, GA_Disabled, FALSE,
                                  GTST_String, (ULONG)(S.vals[sel][0] ? S.vals[sel] : env), TAG_DONE);
            } else if ((id == V_SET || id == V_VALUE || id == V_DEFAULT) && sel >= 0) {
                if (id == V_DEFAULT) {
                    S.vals[sel][0] = 0;
                    DeleteVar((STRPTR)vars[sel].name, GVF_GLOBAL_ONLY);
                    GT_SetGadgetAttrs(vg[2], vw, NULL, GTST_String, (ULONG)"", TAG_DONE);
                } else {
                    STRPTR v;
                    GT_GetGadgetAttrs(vg[2], vw, NULL, GTST_String, (ULONG)&v, TAG_DONE);
                    copystr(S.vals[sel], trim((char *)v), VALLEN);
                    if (strchr(S.vals[sel], '"'))
                        S.vals[sel][0] = 0;
                }
                GT_SetGadgetAttrs(vg[1], vw, NULL, GTLV_Labels, ~0, TAG_DONE);
                build_var_list();
                GT_SetGadgetAttrs(vg[1], vw, NULL, GTLV_Labels, (ULONG)&var_list,
                                  GTLV_Selected, sel_row < var_n ? sel_row : ~0,
                                  GTLV_MakeVisible, sel_row, TAG_DONE);
                if (sel_row >= var_n || var_map[sel_row] != sel) {
                    sel = sel_row = -1;         /* it left the "set now" filter */
                    GT_SetGadgetAttrs(vg[2], vw, NULL, GTST_String, (ULONG)"",
                                      GA_Disabled, TRUE, TAG_DONE);
                }
            } else if (id == V_CLEARALL) {
                for (i = 0; i < NVARS; i++)
                    S.vals[i][0] = 0;
                GT_SetGadgetAttrs(vg[1], vw, NULL, GTLV_Labels, ~0, TAG_DONE);
                build_var_list();
                GT_SetGadgetAttrs(vg[1], vw, NULL, GTLV_Labels, (ULONG)&var_list,
                                  GTLV_Selected, ~0, TAG_DONE);
                sel = sel_row = -1;
                GT_SetGadgetAttrs(vg[2], vw, NULL, GTST_String, (ULONG)"", GA_Disabled, TRUE, TAG_DONE);
                SHOW_INFO("All launcher values cleared (ENV: itself is unchanged).");
            }
        }
    }
#undef SHOW_INFO
    {
        struct TextFont *f = vw->RPort->Font;
        CloseWindow(vw);
        if (f)
            CloseFont(f);
    }
    FreeGadgets(vlist);
    SetWindowPointer(win, WA_Pointer, 0, TAG_DONE);
    save_prefs();
    draw_hint();
}

/* ---- Main loop --------------------------------------------------------- */

static void set_check(UWORD id, LONG *val, UWORD code)
{
    (void)id;
    *val = code ? 1 : 0;
}

static void forget_selected(LONG *item)
{
    int j;
    if (*item < 0 || *item >= S.nrecent) {
        set_status("Select an entry in the recent list first.");
        return;
    }
    for (j = *item; j < S.nrecent - 1; j++)
        strcpy(S.recent[j], S.recent[j + 1]);
    S.nrecent--;
    *item = -1;
    refresh_recent();
    save_prefs();
    set_status("Removed from the recent list.");
}

int main(void)
{
    BOOL done = FALSE;
    ULONG last_secs = 0, last_micros = 0;
    LONG last_item = -1;

    GadToolsBase = OpenLibrary("gadtools.library", 39);
    AslBase = OpenLibrary("asl.library", 38);
    if (!GadToolsBase || !AslBase) {
        if (GadToolsBase) CloseLibrary(GadToolsBase);
        if (AslBase) CloseLibrary(AslBase);
        return 20;
    }
    defaults();
    load_prefs();
    if (S.image[0])
        strcpy(status_text, "Press Start to run the image, or pick another.");
    if (!open_window()) {
        close_window();
        CloseLibrary(AslBase);
        CloseLibrary(GadToolsBase);
        return 20;
    }

    while (!done) {
        struct IntuiMessage *m;
        WaitPort(win->UserPort);
        while (!done && (m = GT_GetIMsg(win->UserPort)) != NULL) {
            ULONG cls = m->Class;
            UWORD code = m->Code;
            struct Gadget *g = (struct Gadget *)m->IAddress;
            ULONG secs = m->Seconds, micros = m->Micros;
            GT_ReplyIMsg(m);

            switch (cls) {
            case IDCMP_CLOSEWINDOW:
                done = TRUE;
                break;
            case IDCMP_REFRESHWINDOW:
                GT_BeginRefresh(win);
                GT_EndRefresh(win, TRUE);
                break;
            case IDCMP_VANILLAKEY:
                switch (code) {
                case 27: case 'q': case 'Q': done = TRUE; break;
                case 's': case 'S': case 13: run_game(); break;
                case 'p': case 'P': pick_image(); break;
                case 'r': case 'R': read_strings(); vars_window(); break;
                case 'v': case 'V':
                    read_strings();
                    set_status(save_prefs() ? "Settings saved." : "Could not save the settings.");
                    break;
                case 'f': case 'F':
                    forget_selected(&last_item);
                    break;
                }
                break;
            case IDCMP_GADGETUP:
                switch (g->GadgetID) {
                case G_PICK:   pick_image(); break;
                case G_VARS:   read_strings(); vars_window(); break;
                case G_START:  run_game(); break;
                case G_QUIT:   done = TRUE; break;
                case G_SAVE:
                    read_strings();
                    set_status(save_prefs() ? "Settings saved." : "Could not save the settings.");
                    break;
                case G_RECENT:
                    if (code < S.nrecent) {
                        set_image(S.recent[code]);
                        if ((LONG)code == last_item &&
                            DoubleClick(last_secs, last_micros, secs, micros)) {
                            last_item = -1;
                            run_game();
                            break;
                        }
                        set_status("Press Start (or double-click) to run it.");
                    }
                    last_item = code;
                    last_secs = secs;
                    last_micros = micros;
                    break;
                case G_FORGET:
                    forget_selected(&last_item);
                    break;
                case G_CORE:   S.core = code; break;
                case G_GPU:    S.gpu = code; break;
                case G_PAD:    S.pad = code; break;
                case G_BIOS:
                    S.bios = code;
                    GT_SetGadgetAttrs(gad[G_BIOSFILE], win, NULL, GA_Disabled, S.bios, TAG_DONE);
                    break;
                case G_SOUND:  S.sound = code; break;
                case G_DITHER: S.dither = code; break;
                case G_INTERP: S.interp = code; break;
                case G_LIMIT:    set_check(G_LIMIT, &S.limit, code); break;
                case G_BILINEAR: set_check(G_BILINEAR, &S.bilinear, code); break;
                case G_BOOTBIOS: set_check(G_BOOTBIOS, &S.bootbios, code); break;
                case G_NTSC:     set_check(G_NTSC, &S.ntsc, code); break;
                case G_FPS:      set_check(G_FPS, &S.fps, code); break;
                case G_WINDOW:   set_check(G_WINDOW, &S.window, code); break;
                case G_VSYNC:    set_check(G_VSYNC, &S.vsync, code); break;
                case G_MC1:      set_check(G_MC1, &S.mc1, code); break;
                case G_MC2:      set_check(G_MC2, &S.mc2, code); break;
                }
                break;
            }
        }
    }
    read_strings();
    save_prefs();
    close_window();
    CloseLibrary(AslBase);
    CloseLibrary(GadToolsBase);
    return 0;
}
