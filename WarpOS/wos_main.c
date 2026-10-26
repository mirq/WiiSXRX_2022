/*
 * wos_main.c -- WiiStation for WarpOS: the program's entry point, settings
 * and the emulator's Sys* callbacks.
 *
 * Takes the place of Gamecube/GamecubeMain.cpp, without the menu: the image
 * comes from the command line or an ASL requester, settings from
 * PROGDIR:wiistation.cfg (the Wii's settingsRX2022.cfg format), then ENV:
 * variables, then key=value arguments, each overriding the one before.
 *
 *   WiiStation [image] [Key=value ...]
 *   e.g. WiiStation DH1:psx/game.cue Core=2 gpuPlugin=2
 *
 * Everything runs on one task: the CPU core's Execute() returns when `stop`
 * is set (quit, or a save-state request handled here between runs).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "../psxcommon.h"
#include "../r3000a.h"
#include "../plugins.h"
#include "../misc.h"
#include "../sio.h"
#include "../cdrom.h"
#include "../gpu.h"
#include "../Gamecube/wiiSXconfig.h"
#include "../Gamecube/DEBUG.h"
#include "../Gamecube/fileBrowser/fileBrowser.h"
#include "../Gamecube/GamecubePlugins.h"

#include "wos_sys.h"
#include "wos_files.h"

void setSpuInterpolation(int spuInterpolation);                         /* spu.c */
void plugin_call_rearmed_cbs(unsigned long autoDwActFixes, int cfgUseDithering);  /* wos_draw.c */
#include "wos_input.h"
#include "wos_video.h"

/* ---- Settings: the globals Gamecube/wiiSXconfig.h declares ------------ */

fileBrowser_file isoFile;
fileBrowser_file cddaFile;
fileBrowser_file subFile;
fileBrowser_file *biosFile = NULL;

PcsxConfig Config;
char dynacore;
char biosDevice;
char LoadCdBios = 0;
char frameLimit[2];
char frameSkip;
char useDithering;
char spuInterpolation;
char showFPSonScreen;
char printToScreen;
char menuActive;
char saveEnabled;
char creditsScrolling;
char nativeSaveDevice;
char saveStateDevice;
char autoSave;
char screenMode = 0;
char videoMode = 0;
char fileSortMode = 1;
char padAutoAssign;
char padType[10];
char padAssign[10];
char padLightgun[10];
char rumbleEnabled;
char loadButtonSlot;
char controllerType;
char numMultitaps;
char lang = 0;
char fastLoad = 0;
char originalMode = 0;
char displayModeChanged = 0;
char bilinearFilter = 1;
char trapFilter = 1;
char interlacedMode = 0;
char deflickerFilter = 1;
char lightGun = 0;
char memCard[2];
char forceNTSC = 0;
char gpuPlugin = 0;

int  stop = 0;
int  g_emu_resetting;
int  framesdone = 0;

#define PATH_LEN 256
static char s_bios_file[PATH_LEN] = "SCPH1001.BIN";
static char s_bios_dir[PATH_LEN]  = "PROGDIR:bios";
static char s_image[PATH_LEN];

#define CONFIG_PATH  "PROGDIR:wiistation.cfg"
#define SAVE_DIR     "PROGDIR:memcards"

static struct {
    const char *key;
    char *value;            /* a char, or a PATH_LEN string when max == STR */
    char  min, max;
} OPTIONS[] = {
#define STR 127
    { "Audio",          &audioEnabled,     AUDIO_DISABLE, AUDIO_ENABLE },
    { "Interpolation",  &spuInterpolation, SIMPLE_INTERPOLATION, GAUSSI_INTERPOLATION },
    { "FPS",            &showFPSonScreen,  0, 1 },
    { "Core",           &dynacore,         DYNACORE_DYNAREC, DYNACORE_DYNAREC_OLD },
    { "BootThruBios",   &LoadCdBios,       BOOTTHRUBIOS_NO, BOOTTHRUBIOS_YES },
    { "LimitFrames",    &frameLimit[1],    FRAMELIMIT_NONE, FRAMELIMIT_AUTO },
    { "SkipFrames",     &frameSkip,        0, 1 },
    { "Dithering",      &useDithering,     USEDITHER_NONE, USEDITHER_ALWAYS },
    { "ControllerType", &controllerType,   CONTROLLERTYPE_STANDARD, CONTROLLERTYPE_ANALOG },
    { "BilinearFilter", &bilinearFilter,   0, 1 },
    { "Memcard0",       &memCard[0],       MEMCARD_DISABLE, MEMCARD_ENABLE },
    { "Memcard1",       &memCard[1],       MEMCARD_DISABLE, MEMCARD_ENABLE },
    { "ForceNTSC",      &forceNTSC,        0, 1 },
    { "gpuPlugin",      &gpuPlugin,        OLD_SOFT, OPEN_GX },
    { "BiosDevice",     &biosDevice,       BIOSDEVICE_HLE, BIOSDEVICE_USB },
    { "BiosFile",       s_bios_file,       STR, STR },
    { "BiosDir",        s_bios_dir,        STR, STR },
};

/* One "key = value" (or key:value, key value); 0 when the key is unknown. */
static int set_option(char *kv)
{
    char *vs = kv, *end;
    unsigned i;

    while (*vs && *vs != ' ' && *vs != '\t' && *vs != ':' && *vs != '=')
        ++vs;
    if (!*vs)
        return 0;
    *(vs++) = 0;
    while (*vs == ' ' || *vs == '\t' || *vs == ':' || *vs == '=')
        ++vs;
    end = vs + strlen(vs);
    while (end > vs && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
        *--end = 0;

    for (i = 0; i < sizeof(OPTIONS) / sizeof(OPTIONS[0]); i++) {
        if (strcasecmp(OPTIONS[i].key, kv))
            continue;
        if (OPTIONS[i].max == STR) {
            if (*vs == '"') {
                vs++;
                if ((end = strchr(vs, '"')) != NULL)
                    *end = 0;
            }
            strncpy(OPTIONS[i].value, vs, PATH_LEN - 1);
            OPTIONS[i].value[PATH_LEN - 1] = 0;
        } else {
            int v = atoi(vs);
            if (v >= OPTIONS[i].min && v <= OPTIONS[i].max)
                *OPTIONS[i].value = (char)v;
        }
        return 1;
    }
    return 0;
}

static void read_config(const char *path)
{
    char line[PATH_LEN + 64];
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f))
        if (line[0] != '#' && line[0] != ';')
            set_option(line);
    fclose(f);
}

/* WSX_<Key> environment variables, e.g. SetEnv WSX_Core 1. */
static void read_env(void)
{
    char kv[PATH_LEN + 64];
    unsigned i;
    for (i = 0; i < sizeof(OPTIONS) / sizeof(OPTIONS[0]); i++) {
        char name[64];
        const char *v;
        snprintf(name, sizeof(name), "WSX_%s", OPTIONS[i].key);
        if ((v = getenv(name)) != NULL && *v) {
            snprintf(kv, sizeof(kv), "%s=%s", OPTIONS[i].key, v);
            set_option(kv);
        }
    }
}

static void default_settings(void)
{
    audioEnabled     = 1;
    spuInterpolation = SIMPLE_INTERPOLATION;
    showFPSonScreen  = 0;
    printToScreen    = 0;
    frameLimit[1]    = FRAMELIMIT_AUTO;
    frameSkip        = 0;
    useDithering     = USEDITHER_DEFAULT;
    autoSave         = AUTOSAVE_ENABLE;
    dynacore         = DYNACORE_DYNAREC_OLD;   /* the ppc/ dynarec; no Lightrec here */
    biosDevice       = BIOSDEVICE_SD;          /* "a BIOS file"; HLE if it is missing */
    memCard[0]       = MEMCARD_ENABLE;
    memCard[1]       = MEMCARD_ENABLE;
    controllerType   = CONTROLLERTYPE_STANDARD;
    gpuPlugin        = OPEN_GX;                /* GlesGpu on MiniGL */
    menuActive       = 0;

    memset(&Config, 0, sizeof(PcsxConfig));
    strcpy(Config.Net, "Disabled");
    Config.PsxOut = 1;
    Config.HLE = 1;
    Config.Xa = 0;
    Config.Cdda = 0;
    Config.cycle_multiplier = CYCLE_MULT_DEFAULT;
    Config.PsxAuto = 1;
    Config.GpuListWalking = -1;
    Config.FractionalFramerate = -1;
    strcpy(Config.PatchesDir, "PROGDIR:ppf/");
}

static void apply_settings(void)
{
    if (dynacore == DYNACORE_DYNAREC)      /* Lightrec is not built */
        dynacore = DYNACORE_DYNAREC_OLD;
#ifndef WOS_HAVE_DYNAREC
    dynacore = DYNACORE_INTERPRETER;
#endif
#ifndef WOS_HAVE_GLES
    if (gpuPlugin == OPEN_GX)
        gpuPlugin = OLD_SOFT;
#endif
    Config.Cpu = dynacore;
    frameLimit[0] = frameLimit[1];
    setSpuInterpolation(spuInterpolation);
    if (gpuPlugin == NEW_SOFT)      gpuPtr = &newSoftGpu;
#ifdef WOS_HAVE_GLES
    else if (gpuPlugin == OPEN_GX)  gpuPtr = &glesGpu;
#endif
    else                            gpuPtr = &oldSoftGpu;
}

/* ---- The BIOS --------------------------------------------------------- */

static void bios_init(void)
{
    static fileBrowser_file bios;

    memset(&bios, 0, sizeof(bios));
    wos_join_path(bios.name, sizeof(bios.name), s_bios_dir, s_bios_file);
    biosFile = &bios;
    if (biosDevice != BIOSDEVICE_HLE && wos_file_open(&bios) == 0 && bios.size >= 0x80000) {
        Config.HLE = BIOS_USER_DEFINED;
        wos_log("BIOS: %s", bios.name);
    } else {
        Config.HLE = BIOS_HLE;
        biosDevice = BIOSDEVICE_HLE;
        wos_log("BIOS: %s not found, using HLE", bios.name);
    }
}

/* ---- The plugins: linked in, looked up by name ------------------------ */

/* The CDR slot stays empty: LoadCDRplugin() takes cdriso.c's functions
 * directly, and its table entry (pointer variables) is no C constant. */
PluginTable plugins[] = {
    PLUGIN_SLOT_0,
    PLUGIN_SLOT_1,
    PLUGIN_SLOT_2,
    EMPTY_PLUGIN,
    PLUGIN_SLOT_4,
    PLUGIN_SLOT_6,
    PLUGIN_SLOT_7
};

void *SysLoadLibrary(char *lib)
{
    int i;
    for (i = 0; i < (int)(sizeof(plugins) / sizeof(plugins[0])); i++)
        if (plugins[i].lib != NULL && !strcmp(lib, plugins[i].lib))
            return (void *)(intptr_t)i;
    return NULL;
}

void *SysLoadSym(void *lib, char *sym)
{
    PluginTable *plugin = plugins + (int)(intptr_t)lib;
    int i;
    for (i = 0; i < plugin->numSyms; i++)
        if (plugin->syms[i].sym && !strcmp(sym, plugin->syms[i].sym))
            return plugin->syms[i].pntr;
    return NULL;
}

void  SysCloseLibrary(void *lib) { (void)lib; }
char *SysLibError(void) { return NULL; }
void  SysRunGui(void) {}

/* ---- Sys* callbacks --------------------------------------------------- */

void SysPrintf(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    wos_log("%s", msg);
}

void SysMessage(char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    wos_log("[msg] %s", msg);
}

void psxCpuInit(void)
{
#ifdef WOS_HAVE_DYNAREC
    if (Config.Cpu != DYNACORE_INTERPRETER)
        psxCpu = &psxRec;
    else
#endif
        psxCpu = &psxInt;
    psxCpu->Init();
}

int SysInit(void)
{
    Config.Cpu = dynacore;
    if (psxInit() < 0)
        return -1;
    LoadPlugins();
    if (OpenPlugins() < 0)
        return -1;
    return 0;
}

extern void pl_timing_prepare(int is_pal_);

void SysReset(void)
{
    g_emu_resetting = 1;
    pl_timing_prepare(Config.PsxType);
    psxReset();
    g_emu_resetting = 0;
}

void SysClose(void)
{
    psxShutdown();
    ClosePlugins();
    ReleasePlugins();
}

/* What SysUpdate saw that the main loop must act on outside Execute(). */
static unsigned s_pending_actions;
static int      s_auto_frames;          /* WSX_AUTOFRAMES: quit after this many */
static uint64_t s_start_us;

/* Once a PSX frame (psxcounters.c, at the vertical blank). */
void SysUpdate(void)
{
    unsigned act;

    framesdone++;
    act = wos_input_poll(wos_video_window());
    if (act & WOS_ACT_FASTFWD) {
        frameLimit[0] = frameLimit[0] ? FRAMELIMIT_NONE : FRAMELIMIT_AUTO;
        act &= ~WOS_ACT_FASTFWD;
    }
    if (s_auto_frames && framesdone >= s_auto_frames)
        act |= WOS_ACT_QUIT;
    if (act) {
        s_pending_actions |= act;
        stop = 1;
    }
    wos_video_frame_stats();
    wos_log_tick();
}

void go(void)
{
    Config.PsxOut = 0;
    stop = 0;
    if (gpuPtr == &newSoftGpu)
        plugin_call_rearmed_cbs(Config.hacks.dwActFixes, useDithering);
    psxCpu->Execute();
}

/* ---- Loading a game --------------------------------------------------- */

static int load_image(const char *path)
{
    fileBrowser_file f;

    memset(&f, 0, sizeof(f));
    strncpy(f.name, path, sizeof(f.name) - 1);
    if (wos_file_open(&f) != 0) {
        wos_log("cannot open %s", path);
        return -1;
    }
    memset(&isoFile, 0, sizeof(isoFile));
    memset(&cddaFile, 0, sizeof(cddaFile));
    memset(&subFile, 0, sizeof(subFile));
    memcpy(&isoFile, &f, sizeof(f));

    if (SysInit() < 0) {
        wos_log("SysInit failed");
        return -1;
    }
    if (strstr(path, ".EXE") || strstr(path, ".exe")) {
        SysReset();
        Load(&isoFile);
    } else {
        CheckCdrom();
        SysReset();
        LoadCdrom();
    }
    if (autoSave == AUTOSAVE_ENABLE) {
        saveFile_init(saveFile_dir);
        LoadMcd(1, saveFile_dir);
        LoadMcd(2, saveFile_dir);
        saveFile_deinit(saveFile_dir);
    }
    wos_log("loaded %s (id %.9s, %s)", path, CdromId, Config.PsxType ? "PAL" : "NTSC");
    return 0;
}

static void save_memcards(void)
{
    extern char mcd1Written, mcd2Written;
    if (autoSave != AUTOSAVE_ENABLE)
        return;
    if (mcd1Written) { SaveMcd(1, saveFile_dir); mcd1Written = 0; }
    if (mcd2Written) { SaveMcd(2, saveFile_dir); mcd2Written = 0; }
}

static void run(void)
{
    for (;;) {
        s_pending_actions = 0;
        go();
        save_memcards();
        if (s_pending_actions & WOS_ACT_QUIT)
            break;
        if (s_pending_actions & WOS_ACT_SAVESTATE)
            wos_log("save state: %s", SaveState() == 0 ? "ok" : "failed");
        if (s_pending_actions & WOS_ACT_LOADSTATE)
            wos_log("load state: %s", LoadState() == 0 ? "ok" : "failed");
    }
}

/* ---- main ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int i, rc = 0;
    uint64_t secs;

    if (wos_sys_init() != 0) {
        wos_sys_shutdown();
        return 20;
    }
    wos_enter_progdir();

    default_settings();
    read_config(CONFIG_PATH);
    read_env();
    for (i = 1; i < argc; i++) {
        char arg[PATH_LEN + 64];
        strncpy(arg, argv[i], sizeof(arg) - 1);
        arg[sizeof(arg) - 1] = 0;
        if (!strchr(arg, '=') || !set_option(arg))
            strncpy(s_image, argv[i], sizeof(s_image) - 1);
    }
    apply_settings();
    s_auto_frames = wos_env_int("WSX_AUTOFRAMES", 0);

    if (!s_image[0] && !wos_request_image(s_image, sizeof(s_image))) {
        wos_log("no image chosen");
        goto out;
    }
    wos_log("image %s core %d gpu %d", s_image, (int)dynacore, (int)gpuPlugin);

    wos_files_install(SAVE_DIR, s_bios_dir);
    bios_init();
    wos_input_init();
    if (wos_video_open() != 0) {
        wos_log("cannot open the display");
        rc = 20;
        goto out;
    }

    if (load_image(s_image) == 0) {
        s_start_us = wos_now_us();
        run();
        secs = (wos_now_us() - s_start_us) / 1000u;
        wos_log("%d frames in %lu ms", framesdone, (unsigned long)secs);
        SysClose();
    } else {
        rc = 10;
    }

    wos_video_close();
    wos_input_shutdown();
out:
    wos_leave_progdir();
    wos_sys_shutdown();
    return rc;
}
