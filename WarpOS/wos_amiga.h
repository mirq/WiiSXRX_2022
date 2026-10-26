/*
 * wos_amiga.h -- every Amiga header the WarpOS build uses, in one place.
 *
 * AmigaOS structures are laid out for the 68k (2-byte alignment); PPC GCC
 * aligns to 4, so every OS header is read inside #pragma pack(push, 2). Include
 * this after the emulator's own headers, never inside them.
 */
#ifndef WOS_AMIGA_H
#define WOS_AMIGA_H

#pragma pack(push, 2)
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <devices/inputevent.h>
#include <libraries/asl.h>
#include <libraries/lowlevel.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/asl.h>
#include <proto/lowlevel.h>
#include <powerpc/powerpc.h>
#include <powerpc/powerpc_protos.h>
#pragma pack(pop)

/* exec, dos and PowerPCBase come from startupwos.o; the rest wos_sys.c opens. */
extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase       *GfxBase;
extern struct Library       *AslBase;
extern struct Library       *LowLevelBase;

#endif
