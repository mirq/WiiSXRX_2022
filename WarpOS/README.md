# WiiStation for WarpOS

The WiiSXRX_2022 PlayStation emulator ("WiiStation") as a native WarpOS
program: the PPC dynarec on the PowerPC card, the GlesGpu hardware renderer
on a Radeon 9200 through the MiniGL R200 stack, AHI sound, the keyboard and
the joystick ports as PSX pads. There is no in-game menu: WiiStationGUI, a
Workbench launcher, picks the disc image and sets the options and ENV:
variables; the command line and an ASL file requester work as well.

## What it needs

| Item | Notes |
|---|---|
| A PowerPC card with WarpOS (603e or later) | built for `-mcpu=603e`, runs on every WarpOS board |
| Radeon 9200 on a Prometheus, Picasso96 | the MiniGL R200 stack: `minigl_ppc.dll`, `minigl_ppc_host`, `LIBS:minigl.library`, `Radeon9200.chip` + `Prometheus.card` |
| `minigl_ppc.dll` from MiniGL branch `wiistation` (`mirq/minigl_wip`) | adds B-F blending and the 2x texture scale; an older ABI-5 DLL also works, with approximated blending (see below) |
| AHI | optional; without it the emulator plays silent |
| lowlevel.library | optional; for joysticks and CD32 pads |
| a PSX BIOS (e.g. `SCPH1001.BIN`) | optional; without one the HLE BIOS runs, which fewer games accept |

## Layout

```
WiiStation/
    WiiStation          the program (its icon has the 1 MB stack it needs)
    WiiStationGUI       the launcher (68k, GadTools): start this one
    WiiStation-R200     launch script (Stack 1048576, then WiiStation)
    WiiStation-Vars.txt every ENV: variable and what it does
    minigl_ppc.dll      MiniGL R200, PPC side
    minigl_ppc_host     MiniGL R200, 68k side
    wiistation.cfg      settings
    bios/               BIOS files
    memcards/           memory cards (<game id>.mcd, slot2.mcd), written at quit
    savestates/         save states (F5 / F7)
    wiistation.log      the log (written every 2 s and at exit)
```

Create `memcards/` and `savestates/` yourself: the program does not make
directories.

## Running

Cold boot, `C:InitPPC` once (if your WarpOS setup needs it), then
double-click **WiiStationGUI**:

- **Disc image**: type a path, press *Pick...* for a file requester, or click
  an entry of the *Recent* list (double-click starts it; *Forget* removes it).
- The settings column and the boxes are the emulator's options: CPU core,
  renderer, pads, BIOS, sound, frame limiter, filtering, memory cards...
- **Variables...** lists every ENV: variable WiiStation and MiniGL read,
  grouped (Display, CPU speed, Renderer, CD, Diagnosis, MiniGL...), each with
  its default, its current ENV: value and an explanation. A value set there
  applies to every run started from the launcher until you set it back:
  it goes into ENV: while the game runs and the old value comes back
  afterwards. *Default* removes the variable from ENV: altogether. The same list is in
  `WiiStation-Vars.txt`.
- **Extra ENV:** takes anything else as `NAME=value NAME2=value`, also for
  the run only.
- **Start** (or Return) runs the game; the launcher waits and comes back
  when you quit with Esc. Settings are kept in `WiiStationGUI.prefs`.

The launcher writes `T:WiiStationGo` (`CD` to its drawer and
`Execute WiiStation-R200 "<image>" Key=value ...`) and clears one-shot
diagnosis variables (`WSX_QUIT`, `WSX_LOADSTATE`, `WSX_AUTOFRAMES`...) left in
ENV: by an earlier run.

From a Shell, in the WiiStation drawer:

```
Execute WiiStation-R200 DH1:psx/Game.cue
Execute WiiStation-R200 DH1:psx/Game.chd Core=1 gpuPlugin=0
Execute WiiStation-R200              ; an ASL requester asks for the image
```

Images: `.cue`/`.bin`, `.iso`, `.img`/`.ccd`/`.sub`, `.mds`/`.mdf`, `.toc`,
`.pbp`, `.cbn`, `.chd`, and PSX `.exe` files.

WiiStation sets MiniGL's variables itself when they are not set: it removes
`MINIGL_CTILE` (GlesGpu reads the framebuffer back, and a tiled render
target reads back scrambled), sets `MGLPPC_CPEMIT=1` (needed for MiniGL's
extensions), `MGLPPC_CPPRESENT=1` (the back buffer keeps its pixels after a
present; some games draw on top of the last frame), `MGLPPC_HOSTPRI=-1` and
the short TCL vertices (`MGLPPC_TCL6`, `MGLPPC_TCL5`). MiniGL starts
`minigl_ppc_host` itself.

## Controls

| PSX | Keyboard | CD32 pad / joystick (port 1 = pad 1, port 0 = pad 2) |
|---|---|---|
| D-pad | cursor keys | stick / D-pad |
| Cross, Circle | X, C | red, blue (fire 1, 2) |
| Square, Triangle | Z, S | green, yellow |
| L1, R1 | Q, W | reverse, forward |
| L2, R2 | 1, 2 | -- |
| Start | Return / Enter | play |
| Select | Space | play + both shoulders |

Esc quits (memory cards are saved), F5 saves the state, F7 loads it, F10
switches the frame limiter off and on, F8 grabs a diagnosis frame.

The joystick ports are read only with `WSX_JOYPORTS=1` (Variables window,
Sound/Pads): each read is a trip to the 68k of about 5 ms.

## Settings

`PROGDIR:wiistation.cfg` (format and keys of the Wii's `settingsRX2022.cfg`),
then `WSX_<Key>` ENV: variables, then `Key=value` arguments (the launcher
passes its settings this way); see the sample file.

Everything else is an ENV: variable: about 90 of them, from display size
and sound rate to A/B switches for the speed work, diagnosis and MiniGL.
`WiiStation-Vars.txt` lists them all with defaults and explanations (made
from `WarpOS/launcher/variables.txt`, which is also compiled into the
launcher's Variables window). The defaults are the fast, tested settings.

## The renderer and MiniGL

GlesGpu was adapted to the Wii through a fork of opengx whose GL calls are
PSX-specific (two texture planes split by the STP bit, primitives drawn in up
to two passes with the PSX blend modes). `wos_gl.c` gives those calls the
same meaning over MiniGL; `wos_gx.h` turns the GX framebuffer copies of the
VRAM readback into `glReadPixels`. Two PSX operations need MiniGL extensions
(MiniGL branch `wiistation`, active with `MGLPPC_CPEMIT=1`):

- `GL_MGL_texture_env_rgb_scale`: texture x colour x 2 (PSX colours above
  0x80 brighten). Without it colours clamp at 1.0 before the multiply.
- `GL_EXT_blend_subtract`: PSX blend mode B - F. Without it B x (1 - F).

The log's `[gl]` line says which are in use.

## Building

On Linux, with ppc-amigaos-gcc 8.4 (adtools) and the MOS2WOS WarpOS SDK
overlay, whose paths come from MiniGL's `Makefile.wos.gcc.common`:

```
make -f WarpOS/Makefile.warpos -j$(nproc)          # -> build/warpos/WiiStation
make -f WarpOS/Makefile.warpos MGL_DIR=/path/to/MiniGL
```

The link is a relocatable ELF checked for the WarpOS contract (no undefined
or COMMON symbols, four relocation types, `_start` at `.text+0`);
The launcher is a 68k program (bebbo's m68k-amigaos-gcc, libnix):

```
make -C WarpOS/launcher                     # -> build/warpos/WiiStationGUI
python3 tools/mkicon/mkicon.py WiiStation.info 1048576   # the icons
```

`tools/elf2warpos/elf2warpos.py` (from OpenSWOS) converts it to the hunk
executable on Linux, so no Elf2Exe2 run on the Amiga is needed.
`build/warpos/WiiStation.debug.elf` keeps the symbols for `addr2line`
against a crash address.

The Wii build is unchanged: `__WARPOS__` and `PSX_HOST_BIG_ENDIAN_PPC`
select everything WarpOS-specific.

## Files

| File | Role |
|---|---|
| `wos_main.c` | `main`, settings, the `Sys*` callbacks, loading a game |
| `wos_sys.c` | clock, caches, buffered log, PROGDIR:, ASL requester, library bases |
| `wos_video.c` | the MiniGL context; the software GPUs' frame as one 565 upload; pacing, stats |
| `wos_draw.c` | what `SoftGPU/drawGX.c` is on the Wii: vout callbacks, frame limiter |
| `wos_gl.c`, `wos_gl.h`, `wos_gx.h` | GlesGpu's GL and GX over MiniGL |
| `wos_input.c` | keyboard (IDCMP_RAWKEY) and joystick ports (ReadJoyPort) |
| `wos_pad.c` | the PSX pad protocol (SSSPSX's) over `wos_input` |
| `wos_files.c` | the fileBrowser interface over stdio |
| `../dfsound/ahi.c` | the SPU's output: an AHI ring, no 68k call per feed |
| `wos_xadecode.c` | the paired-single ADPCM decoders in C |
| `wos_stubs.c`, `wos_debug.c`, `compat/` | what the core takes from Wii-only files |

## State

Runs on a real A4000/68060 with an MPC7410 WarpOS card and a Radeon 9200 on
a Prometheus. Frame rates (full speed is 60, or 50 for PAL games):

| Game | Scene | fps |
|---|---|---|
| Tekken 3 | a fight | ~59.7 |
| Crash Bandicoot | a level | ~59.9 |
| Silent Hill | the town (a 30 Hz game) | 59.8 |
| Ace Combat 3 (PAL) | a flight (a 25 Hz game) | 49.6-50 |
| R-Type Delta | stage 1 | ~59 |

Known gaps: no multitap, lightgun or rumble; no in-emulator menu; per-game
settings files and PPF patches are not loaded yet.
