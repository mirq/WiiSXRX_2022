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

Then compare the destination CRC (`amiga_checksum`) with the CRC on the PC.
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
- `SetEnv WSX_AUTOFRAMES n` quits after n PSX frames.
- `SetEnv WSX_QUIT 1` quits from the bridge. If the CPU loop does not
  return, the program shuts down from the vblank 2 s later.
- Don't use Break or CTRL-C.
- Settings in ENV: are read with `GetVar`; newlib's `getenv()` finds nothing
  under MOS2WOS.

## When it hangs

- The log (`wiistation.log`) is flushed every 2 s.
- If 3 s pass without a vblank, `[watchdog]` lines report the PSX pc.
- For a hang in C code, build with `make ... WOS_CRUMBS=1`. The log then
  gives the address of `wos_crumb[]`, which `amiga_inspect_memory` can read:
  - `[0]` is the last site passed (grep `WOS_CRUMB`).
  - `[1]` is the frame.
  - `[2]` is the PSX pc.
- The log also prints `psxRegs` and `psxM`, so PSX code and registers can be
  read the same way.
- A frozen program can also be a PPC exception. Its requester opens on the
  Workbench screen, behind the MiniGL screen.
- Fixed Wii addresses (`Gamecube/MEM2.h`: `*_LO`) crash with no message.
  The SPU RAM was one of them.

## Layout on the Amiga

- Program drawer: `Work:WiiStation/`.
  - `WiiStation`
  - `WiiStation-R200` (the launch script)
  - `minigl_ppc.dll`: from MiniGL branch `wiistation`, converted on Linux with
    `tools/elf2warpos/elf2warpos.py`. The Oct 21 DLL from `Work:SilentHill WOS`
    is kept as `mgl_sh.dll`, its host as `mglhost_sh`.
  - `minigl_ppc_host`
  - `wiistation.cfg`
  - `bios/`, `memcards/`, `savestates/`
- Test game: Silent Hill, in `Work:silenthill wos`. The path has a space, so
  quote it.
- Log: `Work:WiiStation/wiistation.log`.

## Build

`make -f WarpOS/Makefile.warpos -j$(nproc)` produces `build/warpos/WiiStation`.
It is already converted to a hunk executable, so it needs no Elf2Exe2. Resolve
crash addresses with `addr2line` against `build/warpos/WiiStation.debug.elf`.
