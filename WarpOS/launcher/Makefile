# WiiStationGUI: the 68k launcher (bebbo's m68k-amigaos-gcc, libnix).
CC     := /opt/amiga/bin/m68k-amigaos-gcc
OUT    := ../../build/warpos/WiiStationGUI
VARS   := ../../build/warpos/WiiStation-Vars.txt
CFLAGS := -noixemul -Os -m68020 -Wall -Wno-pointer-sign -fomit-frame-pointer

$(OUT): wsx_launch.c wsx_vars.h
	@mkdir -p $(dir $(OUT))
	$(CC) $(CFLAGS) -s -o $@ $<

clean:
	rm -f $(OUT)

wsx_vars.h $(VARS): variables.txt mkvars.py
	@mkdir -p $(dir $(VARS))
	python3 mkvars.py variables.txt wsx_vars.h $(VARS)
