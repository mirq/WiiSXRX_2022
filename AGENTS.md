# Agent notes — WiiStation WarpOS port (branch `warpos`)

Port details are in `WarpOS/README.md`. Deeper MiniGL/R200 rules are in
`/home/mirek/MiniGL_WOS_V19_mglQ3/AGENTS.md`; they apply here too.

## Target machine

- A real A4000/68060 with an MPC7410 WarpOS card and a Radeon 9200 on a
  Prometheus. It is not an emulator, so never use the `amiga_emulator_*` tools.
- AmigaBridge runs at **`192.168.100.47:2345`**. The old `192.168.1.21` no
  longer applies. The devbench MCP server (`localhost:3000`, profile
  `real-amiga`) connects to it by itself. Don't call `amiga_connect` while it
  is already connected: a second connect left the bridge accepting TCP but
  answering nothing.
- This PC is **`192.168.100.46`** on the same LAN.
- Session rules:
  - Cold boot, then `C:InitPPC` exactly once.
  - Start a fresh `minigl_ppc_host` before every run; the launch script does
    this.
  - Use `Stack 1048576`.
  - Run every bridge file or DOS operation one at a time, never in parallel.
  - Check the CRC32 after every transfer.
  - Keep Amiga-side names to 16 characters or fewer.

## Deploying files

Don't push large files (anything over a few KB) through the bridge. Serve them
over HTTP from this PC and fetch them on the Amiga with wget.

```
# PC: the staging directory holds what goes to the Amiga
python3 -m http.server 8090 --bind 192.168.100.46     # in build/warpos/deploy
# Amiga (through amiga_dos_command):
System:C/wget -q -O <dest> http://192.168.100.46:8090/<file>
```

Then compare the destination CRC (`amiga_checksum`) with the CRC on the PC,
and `Protect <dest> +e` for a program: wget can leave the file without the
e bit, and Execute then runs nothing and says nothing.
Small text files (scripts, cfg) may go through the bridge.

**Flush the disk before any reboot.** `amiga_reboot` does not flush DH2:
(`Work:`). Files written minutes before a reboot have come back as the
previous version, even though their CRC matched before the reboot. Run
`Lock DH2: ON` and then `Lock DH2: OFF` after deploying, and check the CRC
again after the boot.

`Elf2Exe2` took a PPC exception converting the MiniGL DLL. The Hyperion
loader starts a DLL as an ordinary WarpOS executable, so
`tools/elf2warpos/elf2warpos.py` converts `minigl_ppc_r200.warpelf` on Linux
as well. This is how `minigl_ppc.dll` was deployed.

## Running and stopping

- Launch with `Execute WiiStation-R200 "<image>" Key=value ...` from
  `Work:WiiStation`. The script hands its whole line to WiiStation, and
  WiiStation joins the words that are not `Key=value` back into the path,
  since Execute drops the quotes.
- MiniGL's library starts `minigl_ppc_host` itself when the host's port is
  missing (MiniGL `R200PPCHostEnsure`, at open: LoadSeg + CreateNewProc,
  pri -1, home dir = the host's drawer), from `$MGLPPC_HOST`,
  `PROGDIR:minigl_ppc_host` or `LIBS:minigl_ppc_host`; `MGLPPC_NOHOSTSTART 1`
  never starts one. So `WiiStation "<image>"` from a Shell or an icon works
  (Stack 1048576), and the script no longer starts a host. WiiStation only
  sets its MiniGL settings (`wos_mgl_settings`, `WarpOS/wos_video.c`). A host
  of its own process served Tekken 3 fights faster than one started through
  `Run` (44 -> 47 fps). Keep `MGLPPC_HOSTPRI` at -1: at 1
  the host starved the bridge and the machine needed a reset.
- Tekken 3 fight benchmark: `savestates/SLUS99999.st0` is now a fight
  (copy `tekfight.st0`; the character select is `tekken.st0`):
  `WSX_LOADSTATE 60`, `WSX_AUTOFRAMES 1000`, no input, read the two
  `[stats]` lines after the load.
- Switches from the 2026-11-03 work (all default on unless noted):
  `WSX_UNSYNC 0` (cache uploads wait for the GPU), `WSX_GLDEFER 0` (main/semi
  atlas planes uploaded at once), `WSX_SOFTFILL 1` (full-screen clears also
  into GlesGpu's VRAM copy), `WSX_ZALWAYS 1` (depth test on with GL_ALWAYS),
  `WSX_MASKGATE 0` (a raised irq holds the event gate open while SR masks
  it), `WSX_TCLARRAY 0` (transformed path through glBegin/glEnd; default 1,
  arrays),
  `WSX_GLFLAT 1` (screen-space MGL_FLATTRIANGLES; default is now 0).
  MiniGL: `MGLPPC_NOINPLACE`, `MGLPPC_POOLSYNCROW`, `MGLPPC_NOTCLFAST`,
  `MGLPPC_NOTCL7`.
- Short TCL vertices (MiniGL `RADEON3D_DRAW_NO_W`/`NO_Z`): WiiStation sets
  `MGLPPC_TCL6 1` (no W, 6 dwords) and `MGLPPC_TCL5 1` (no Z either when the
  depth test is off; untextured draws also drop s,t) unless they are set.
  The vertex stream crosses the Zorro bus, so glEnd follows its size
  (Tekken 3 window 2: 2.97 ms at 7 dwords, 2.25 ms at 5; fights ~49 -> ~51).
  `SetEnv MGLPPC_TCL6 0` for 7 dwords. A new stride must be handled in all
  three emitter sites (`EmitExecuteVertices`, the record stride,
  `Radeon3DEmitVbufDraw`) and on MiniGL's inline path, which some flushes
  take: a missed one makes every draw fail and leaves the host broken.
- `WSX_DEFERMERGE 0`: deferred main/semi atlas rectangles go up one call
  each instead of merged into bounding boxes (`WSX_DEFERSLACK`, 512 extra
  texels). A call costs ~65 us besides ~0.12 us a texel.
- Waiting in MiniGL's in-place pool drain for the client's sync point
  instead of the slot's last use (tried 2026-11-03) stalled the deferred
  uploads to 200 ms a frame and hung the host: keep the drain as it is.
- `stswi`/`dcbz` into the VRAM aperture and `dcbf` on 68k-owned memory
  (an Intuition MsgPort) hang this machine before the first log flush.
- `SetEnv WSX_AUTOFRAMES n` quits after n PSX frames.
- `SetEnv WSX_LOADSTATE n` loads the save state (F5 saves, F7 loads) at
  frame n: the quick way back to a scene. It persists in ENV:, so
  `UnSetEnv WSX_LOADSTATE` afterwards. `WSX_NOGATE 1` / `WSX_NOIDLE 1`
  switch off the event gate and the idle-loop skip, to compare.
- `SetEnv WSX_GLBENCH 1` times MiniGL's immediate mode at startup (draws in
  the shapes `wos_gl.c` uses, each state change on its own, and a mip-level
  check) and logs `[glbench]` lines. For MiniGL's own buckets, build its
  profile DLL with `MGLPPC_PROFILE=1 MGLPPC_AUTOREPORT=1` (own `OBJDIR`); it
  appends to `T:mglprof-live.log` every 150 presents.
- `SetEnv WSX_VLC n` selects how PsyQ's movie bitstream decoder
  (`DecDCTvlc2`, found by its code) runs: 1 (default) as C (`WarpOS/wos_vlc.c`),
  0 on the dynarec, 2 checked: an interpreter runs the routine's own code
  next to the C version, and the `[prof] movie decoder` line counts any
  difference. Tekken 3's build matches; Silent Hill's differs in 26 words.
- Switches for A/B tests of the speed work (all default on):
  - `WSX_FASTMEM 0`: every dynarec load and store through the C accessors.
  - `WSX_DISPATCH 0`: blocks return through `recRun` instead of the
    dispatcher stub.
  - `WSX_GLBATCH 0`: one `glBegin` per primitive on the transformed path.
  - `WSX_GLFLAT 0`: GlesGpu's draws take MiniGL's transformed path instead
    of screen-space `MGL_FLATFAN` (`WarpOS/wos_gl.c`, `draw_flat`).
  - `WSX_HYB lo-hi`: blocks starting in that hex PSX address range run on
    the interpreter, to find translated code that goes wrong.
- `SetEnv WSX_GTEBENCH 1` times the busiest GTE operations warm at startup
  (`[gtebench]`). With WOS_PROFCPU the `[prof] gte ops` line gives each
  operation's share of the GTE time; `gte.c`'s fast paths must stay
  bit-exact (compare against the previous gte.c on random registers).
- The bridge reads memory from the 68060 side: hot PPC data can still sit
  in the PPC's copy-back cache, so `amiga_inspect_memory` of live hardware
  registers or counters can show stale values. Log counters instead.
- `make ... WOS_PROFCPU=1` adds `[prof] cpu split` (GTE and hardware
  register time) and `[bprof]` (the blocks with the most host instructions
  run). The dynarec's speed on the 7410 follows code size, not instructions
  run: the 32 KB instruction cache is the limit, so keep slow paths out of
  line (the shared stubs at the start of the code buffer).
- `SetEnv WSX_PMC 1` programs the 7410's performance counters (supervisor
  mode through WarpOS `Super()`) and logs `[pmc]` lines: per frame, in
  thousands, PMC1-4 for the whole frame and for the gpu, rcnt, irq, gte,
  hw and branch-test stages, what is left (translated code), and the GPU
  commands with the most PMC1 events. `WSX_PMCSEL a,b,c,d` picks the events;
  the default 1,5,5,2 is cycles, L1 instruction misses, L1 data misses,
  instructions completed (checked at startup by the `[pmc]` calibration
  lines). The CPU runs at ~445 MHz.
- `SetEnv WSX_LINK 0`: a j/jal/branch block goes to the dispatcher instead
  of straight to its target's code (through the target's `psxRecLUT` slot,
  when no event is due). Whatever stops a run, or flushes the translations,
  in the middle of a block must `WOS_EVENT_NOW()` so that the next block
  end goes through the branch test and the dispatcher's checks.
- GlesGpu's texture cache starts at 16 pages, the 16 cells of the GL atlas
  (2048 x 512, `WarpOS/wos_gl.c`; MiniGL's direct and pool paths take up to
  4 MiB, `R200_DIRECT_MAX_BYTES`). With 8 pages Tekken 3 fights cleared the
  cache every ~300 frames and dropped to 45-50 fps while it refilled; with
  8 cells plus 8 textures of their own, a live fight switched between them
  70-110 times a frame, each a MiniGL record (~85 us). It grows to 64 for good after 8 clean-ups in 2 s
  (`wos_sorttex_grow`, logs `[tex]`). `SetEnv WSX_SORTTEX n` fixes it at n.
  A 1024 x 1024 atlas hung MiniGL at the first sub-image (before the 4 MiB
  direct limit; MiniGL's pool still takes at most 512 rows); a hang there
  needs a reboot.
- `[gpu] display mode` lines give the game's own mode when it changes:
  Tekken 3 is 368 x 480 interlaced in menus and fights.
- AltiVec on this 7410 (`tools/avbench`, 2026-11-03): WarpOS turns MSR[VEC]
  on at a task's first vector instruction and keeps each task's vector
  registers across task switches and 68k calls (two copies of
  `avbench stress` for 20 s). Two traps, both silent wrong results, not
  crashes: the hunk loader aligns sections to 8 bytes only, so GCC's vector
  constants in `.rodata` (and any static `aligned(16)` data) load the wrong
  16 bytes; and a task's stack is 4-byte aligned (`mod 16 = 12`), so
  `-mabi=altivec` spills and v20-v31 saves land on the wrong address.
  Keep vector constants in memory aligned at run time and call vector code
  through a stack-aligning stub (`tools/avbench/avalign.s`). Measured gains:
  4-bit CLUT x2.0-3.5, 15-bit -> BGRA x2.2, SPU-style mix x4, 4x4 float
  transform x2.1, cached copy x1.5-2.6, RAM copy with `dcbz` x1.9. The
  aperture (Zorro) writes do not get faster (~42 ns a dword either way).
- Where a Tekken 3 fight frame goes (2026-11-03, ~17.5 ms, PMC and
  MiniGL's profile DLL): translated code ~9.2 ms (L1 instruction misses
  ~350k a frame), GlesGpu ~3.4 ms (texture lookup 1-2 ms, ~950 lookups),
  MiniGL ~2 ms (4750 vertices, 1.1 ms of it the bus writes at ~42 ns a
  dword: no CPU work, AltiVec or not, makes that faster; stwbrx against
  stw made no difference there), rcnt/SPU ~0.6 ms, uploads 0.3-1.5 ms
  (bus). The texture search keeps a 1024 x 8 hash of found entries (key:
  bucket, palette key, 16-texel cell; 96% hits) and the palette key memo
  128 x 2 sets (`[prof] texture search hash` line). ~half the primitives
  are quads (sent as 2 triangles).
- `[pmc]` stages nest: `bt` (branch test) contains rcnt and irq; `rest`
  is everything but rcnt/irq/gte/hw, so it contains gpu and bt too.
- The MiniGL host log, profile logs etc. come to the PC with
  `wget --post-file=<file> http://192.168.100.46:8091/<name>` when a
  receiver runs here (a 20-line Python http.server POST handler).
- MiniGL `GL_MGL_palette_lookup` (2026-11-03/04, `glEnable(MGL_PALETTE_LOOKUP)`
  0x7021, `RADEON3D_DRAW_PALETTE`): unit 0 = index texture (red), unit 1 =
  palettes, one a row; the R200 two-pass fragment shader (Mesa
  r200_fragshader.c layout) looks the colour up in hardware. WiiStation's
  path (`wosgl_gp_*`, GlesGpu `wos_gp_select`: 16 index cells of VRAM pages,
  palette rows) is off by default (`WSX_GPUPAL 1`): the index lookup is exact
  (`tools/paltest`, `WSX_GPDEBUG 1` names the hit column/slot per pixel), but
  the palette row cannot reach the shader reliably. In the vertex alpha the
  first pass sees the previous primitive's alpha in screen tiles both touch
  (whole areas of a sprite drew another palette's colours); flat alpha
  shading does not help. As unit 1's coordinate (s1,t1, `MGLPPC_PALVAR`
  0/1, with or without Z) every draw locked the GPU (`DH2:cpfail.log`
  `host_error`, the host then stays stuck: reboot). `WSX_SHOT n` grabs
  present n into `T:shot.ppm` (exact colours, unlike the bridge screenshot);
  `WSX_GPPROBE all` logs every palette-path primitive of frame 290
  (polygon coordinates are before the drawing offset).
- `SetEnv WSX_QUIT 1` quits from the bridge. It stays in ENV: when the run
  ended by itself, and the next run then quits at frame 50: `UnSetEnv` it. If the CPU loop does not
  return, the program shuts down from the vblank 2 s later.
- Don't use Break or CTRL-C.
- Keys from the bridge (`amiga_input_key`) reach the game:
  - Return is Start, X is Cross, Esc quits.
  - Hold a key for a few ticks; the game polls once a frame. AmigaDOS
    `Wait n` is n seconds, not ticks.
- `SetEnv WSX_PMC 1` + `WSX_PMCPROF <cycles>` (e.g. 40000) is a sampling
  profiler: the 7410's performance-monitor interrupt (`EXCF_PERFMON`)
  records pc and lr from present `WSX_PMCPROF_AT` (400) until 65536 samples,
  then writes `T:pcsamp.bin` (header: count, run-time `wos_log`, code
  buffer start/size; samples; then the dynarec's block map, PSX pc -> host
  code). The log also gives `MGLGetDispatchTable`'s address for the DLL.
  Resolve with addr2line against `WiiStation.debug.elf` and a `-g` build of
  the DLL with identical code (`WOS_ARCH_CFLAGS=... -g`, own OBJDIR/WARPELF:
  `.text` compares equal). Samples at 0x50400000/08 with lr 0xfff1306c are
  WarpOS idle (waiting), not work.
- Delay-slot loads (2026-11-04): `iLoadTest` sent every load in a branch
  delay slot through `psxDelayTest`; now only when the target's first
  instruction uses the register (`WSX_DELAYFAST 0`: as before). Fights
  56 -> 58 fps.
- The block exit is `li r4,cycles; lis/lwz r5 (target slot); bl stub; .long pc`:
  the stub reads the pc through lr only on its slow paths, and
  `wos_next_event` off r31.
- MiniGL `DrawTrianglesTclFast` serializes one interleaved client array
  straight into the segment (`MGLPPCSwapTCL5X/6X`, `screenExt`), no
  ScreenVertex staging; `MGLPPC_NODIRECTTCL 1` stages as before.
- `[stats] busy ms a frame` is a histogram of each frame's own time and
  `[spike]` lines (frames of 25 ms+) give the stages (WOS_PROF enum index:
  0 gpu, 3 cdr, 6 irq, 10 texup, 12 sys, 13 input, 15 state, 18 glend,
  21 texsel). Tekken 3 fights: most frames 14-15 ms; the lost fps are
  spikes: texture uploads waiting on the host (10-45 ms) and 68k calls.
- **A PPC -> 68k call costs 1-5 ms here** (GetMsg with the message already
  in the port: ~2.5 ms), whatever the priorities (WiiStation's process at 1
  changed nothing). Keep them off per-frame paths. The log is written by an
  async DOS packet (`WSX_LOGASYNC 0`: fopen each flush, 20-28 ms every 2 s);
  `wos_cdasync` reads 4..32-sector runs (`WSX_CDRUN`) and skips WaitPort
  when the packet's `dp_Res1` shows it answered (`[prof] cd image` lines).
  Never cache-flush/invalidate an exec MsgPort (a reply port's list) from
  the PPC: it hung the machine.
- The upload spikes (stage 10, 10-45 ms) are not pool waits: MiniGL's
  profile DLL shows pool wait 0 and no full copies, but some small
  `FrontendLmPoolCopy` runs ~15x slower than usual (bus contention with the
  68k's disk I/O, likely). `MGLPPC_POOLSLOTS 5` and skipping the in-place drain
  for a retired spare changed nothing. The pool's history is now 128 updates.
- Benchmarks (2026-11-04): t5 Tekken 3 fight (2500 frames) ~59.7 fps;
  t7 Crash (`LOADEREXE.st0`, 1500 frames) ~59.9; t8 Silent Hill
  (`SLUS00707.st0`) 59.8 with the second lane (53.7 before it, 54.6 before the fog fix; a 30 Hz game: the present frames take ~37 ms of
  two). Silent Hill's cost is MiniGL per draw (~580 draws a present,
  textured opaque against untextured semi-transparent, so the blend changes
  with the texturing): `[prof] texture batch ends` and the
  `untextured primitives` lines show it. Next lever there: the emitter's
  full `EmitExecuteState` on every blend/texturing change (only a
  texture-only delta path exists).
- MiniGL: no VRAM read-back after each draw's vertices (`MGLPPC_SEGREAD 1`
  restores it; the publish's flush and ring read-back order them; Silent
  Hill 50.5 -> 54.6). `GL_CLAMP_TO_BORDER` on s (white border,
  `RADEON3D_TEX_BORDER_S`, `PP_BORDER_COLOR` 0x2c14); WiiStation's
  `WSX_UNTEXMERGE 1` uses it to draw untextured primitives inside a
  textured batch of the same blend (off by default: untested, and Silent
  Hill's differ in blend).
- The PSX mask bit lives in the depth buffer (GlesGpu `SetZMask*`: set mask
  z 0.95, else a rising `gl_z`; a mask check is GL_LESS). `WSX_ZALWAYS 0`
  (depth off while not checking) left Silent Hill's trees and fences black:
  it draws foliage as a black textured pass (clears the mask) and then a
  fog quad that checks the mask. `wos_gl.c` now writes depth for every
  primitive from the first mask check on (`[gl] mask bit checked` in the
  log); Tekken 3 and Crash never check and keep the faster path.
- `SetEnv WSX_PRIMPROBE n` logs every primitive of PSX frame n (`[prim]`:
  command/colour, xy before the drawing offset, tpage, clut, uv, mask).
  Silent Hill sends a whole frame by one DMA every other frame: pick an
  odd n (301), an even one logs nothing.
- MiniGL state memo (`radeon3d_emit.c`, `MGLPPC_NOSTATEMEMO 1` off): the
  register list of the last 4 unlit states, replayed through the register
  cache instead of a full `EmitExecuteState`. Silent Hill 51.9 -> 53.7.
- Two lanes in `wos_gl.c`'s arrays path (`WSX_LANES 0` off): when the state
  flips, the new key's primitives wait in a second run drawn after the
  first; a first-key primitive still joins the first run if it overlaps
  (separating-axis test, shared edges allowed) none of the second run's.
  Silent Hill's ground is ~250 pairs a frame of an opaque untextured quad
  under the same quad textured semi-transparent: 54.2 -> 59.8 fps, frame
  grabs identical (`[prof] second lane` line). MiniGL's state memo also
  replays only the registers that differ from the previous memo entry.
  Crash frame grabs differ run to run in the crab's animation: compare
  two references before blaming a change.
- Block linking (`WSX_LINK`) skips the pc store: anything a block calls that
  reads `psxRegs.pc` must get the block's pc stored first. The movie
  decoder hook (`wos_vlc_block`) did not, and Tekken 3's boot showed a
  black intro (audio only) and then ran into its kernel's data (pc
  0x1ca4, then 0x20001). Fixed for the vlc and `WSX_HYB` hooks.
- The PSX drawing area (GP0 E3/E4) is the GL scissor (GlesGpu
  `SetOGLDisplaySettings`, `WSX_DRAWCLIP 0` off): Tekken 3's menu shows
  three entries only through it. Fights now leave their top and bottom
  16 lines black and Crash 12 lines each, as the games' areas say.
- `F8` (diagnosis): the next present into `T:shot.ppm` and the frame after
  next's primitives into the log (`[prim]`), for a scene a save state
  cannot hold (Tekken 3's menu goes to its attract demo).
- Settings in ENV: are read with `GetVar`; newlib's `getenv()` finds nothing
  under MOS2WOS.

## Reading the log

`Type` through the bridge cuts long output at about 4 KB, and the end of the
log is then simply missing; it is not a crash. Read the end with
`amiga_read_file` at an offset (file size from `List ... LFORMAT="%l"`).

The log has three kinds of periodic line:
- `[stats]`: fps, presents, late frames, upload/swap/wait time per frame.
- `[prof]`: microseconds per frame by stage. "rcnt" contains lace/present
  and the SPU, "irq" contains CD reads, and "the rest" is translated code.
- `[pcs]`: the PSX pcs with the most time.

## When it hangs
- `amiga_screenshot` of the MiniGL screen comes out grey with dark edges for
  a correct picture too: it shows geometry, not colours or textures, so
  only the user can confirm the picture.
- MiniGL appends draw-emit failures to `DH2:cpfail.log` (`PUB emit stage=`
  is hex: 48 is the emitter's `FailStage` 72). Check its size after a run
  that changed MiniGL's records.

- The log (`wiistation.log`) is flushed every 2 s (asynchronously: the
  directory shows the old size/date until the run ends).
- If 3 s pass without a vblank, `[watchdog]` lines report the PSX pc.
- For a hang in C code, build with `make ... WOS_CRUMBS=1`. The log then
  gives the address of `wos_crumb[]`, which `amiga_inspect_memory` can read:
  - `[0]` is the last site passed (grep `WOS_CRUMB`).
  - `[1]` is the frame.
  - `[2]` is the PSX pc.
- The log also prints `psxRegs` and `psxM`, so PSX code and registers can be
  read the same way.
- A frozen program can also be a PPC exception. Its requester opens on the
  Workbench screen, behind the MiniGL screen. The log gives the address of
  an exception record (`wos_exc`, `WarpOS/wos_sys.c`) the handler fills.
- A program whose process is gone while its screen stays open called
  `exit()` from somewhere (an `assert` is enough): the atexit hook logs
  `[diag] exit()` with the return addresses up the stack, against the
  run-time address of `wos_log` for `addr2line`. Stores to unmapped
  addresses do not fault on this machine.
- Don't run tests while a large file downloads to `Work:`: the CD image
  reads compete with it for the disk (247 ms a frame of CD reads).
- Fixed Wii addresses (`Gamecube/MEM2.h`: `*_LO`) crash with no message.
  The SPU RAM was one of them.

## Layout on the Amiga

- Program drawer: `Work:WiiStation/`.
  - `WiiStation`
  - `WiiStation-R200` (the launch script)
  - `minigl_ppc.dll`: from MiniGL branch `wiistation` (53d2afb or later:
    direct-path texture pages and partial updates, the CP register cache,
    and the row-pool fix 1bf7e31 without which the Crash compilation froze
    in `glTexSubImage2D`; WiiStation relies on them for 3D speed), converted on Linux with
    `tools/elf2warpos/elf2warpos.py`. The previous build is `mgl_prev.dll`; the
    Oct 21 DLL from `Work:SilentHill WOS` is `mgl_sh.dll`, its host
    `mglhost_sh`.
  - `minigl_ppc_host`
  - `wiistation.cfg`
  - `bios/`, `memcards/`, `savestates/`
- Test game: Silent Hill, `Work:SilentHill WOS/gamedata/SLUS_007.07.bin` (Work:WiiStation/t8 runs it; t5 Tekken 3, t7 Crash). The path has a space, so
  quote it.
- That image (`SLUS_007.07.bin`) was ripped as mode 2 form 1: the
  subheaders are zero and the last 276 bytes of each sector are form-1
  EDC/ECC. Its XA audio is still there in 16 of every sector's 18 sound groups.
  `cdrom.c` (`rip_rebuild`) recognises those sectors and `decode_xa.c` makes up
  the two lost groups; the `[prof] form-1 rip audio` line counts them.
- Log: `Work:WiiStation/wiistation.log`.

## Build

`make -f WarpOS/Makefile.warpos -j$(nproc)` produces `build/warpos/WiiStation`.
Give the user (and take benchmark numbers from) this plain build: `WOS_PROFCPU=1`
and `WOS_CRUMBS=1` are for diagnosis only. Their timers around every GTE
operation and hardware access, and a `dcbst`+`sync` per breadcrumb, cost the
Crash long-view benchmark 48 -> 58 fps. The build schedules for the 7410
(`TUNE := -mtune=7400`) but keeps 603e instructions, so it still runs on
every WarpOS board.
It is already converted to a hunk executable, so it needs no Elf2Exe2. Resolve
crash addresses with `addr2line` against `build/warpos/WiiStation.debug.elf`.
