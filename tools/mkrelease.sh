#!/bin/sh
# Build the WarpOS release archive: build/release/WiiStation-WarpOS-<ver>.lha
# usage: tools/mkrelease.sh <version> <minigl_ppc.dll> <minigl_ppc_host>
set -e
VER=${1:?version}; DLL=${2:?minigl_ppc.dll}; HOST=${3:?minigl_ppc_host}
cd "$(dirname "$0")/.."
make -f WarpOS/Makefile.warpos -j"$(nproc)" >/dev/null
make -C WarpOS/launcher >/dev/null
OUT=build/release; D=$OUT/WiiStation
rm -rf "$OUT"; mkdir -p "$D/bios" "$D/memcards" "$D/savestates"
cp build/warpos/WiiStation build/warpos/WiiStationGUI build/warpos/WiiStation-Vars.txt "$D/"
cp release/WarpOS/WiiStation-R200 release/WarpOS/wiistation.cfg "$D/"
cp "$DLL" "$D/minigl_ppc.dll"; cp "$HOST" "$D/minigl_ppc_host"
cp WarpOS/README.md "$D/ReadMe.md"; cp Docs/License.txt "$D/License.txt"
python3 tools/mkicon/mkicon.py "$D/WiiStation.info" 1048576
python3 tools/mkicon/mkicon.py "$D/WiiStationGUI.info" 16384 play
python3 tools/mkicon/mkicon.py "$OUT/WiiStation.info" 0 drawer
printf '%s\n' "Put your own PlayStation BIOS dump here (SCPH1001.BIN for NTSC-U," \
  "or set BIOS file in WiiStationGUI). Without one the HLE BIOS runs," \
  "which fewer games accept." > "$D/bios/ReadMe.txt"
printf 'Memory cards (<game id>.mcd), written when you quit.\n' > "$D/memcards/ReadMe.txt"
printf 'Save states (F5 saves, F7 loads).\n' > "$D/savestates/ReadMe.txt"
chmod 755 "$D/WiiStation" "$D/WiiStationGUI" "$D/minigl_ppc.dll" "$D/minigl_ppc_host"
(cd "$OUT" && lha aq "WiiStation-WarpOS-$VER.lha" WiiStation.info WiiStation)
(cd "$OUT" && sha256sum "WiiStation-WarpOS-$VER.lha" > "WiiStation-WarpOS-$VER.lha.sha256")
ls -la "$OUT"
