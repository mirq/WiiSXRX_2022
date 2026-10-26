/*
 * wos_stubs.c -- what the emulator core takes from Wii-only files, for the
 * WarpOS build: the scheduler state lightrec.c defines, the menu's flags,
 * libogc's clock, and do-nothing hooks for the GUI and the SD-card thread.
 */
#include <errno.h>
#include <malloc.h>
#include <stdlib.h>

#include "../psxcommon.h"
#include "../r3000a.h"
#include "../cdrom.h"
#include "../mdec.h"
#include "../psxdma.h"
#include "../sio.h"
#include "../spu.h"
#include "../psxcounters.h"
#include "../psxhw.h"
#include "wos_sys.h"

/* ---- lightrec.c's event scheduler (the HLE BIOS uses it) ------------- */

u32 event_cycles[PSXINT_COUNT];
u32 next_interupt;

static void irqNoOp(void) {}
void lightgunInterrupt(void) {}

typedef void (irq_func)();

static irq_func * const irq_funcs[] = {
	[PSXINT_SIO]	= sioInterrupt,
	[PSXINT_CDR]	= cdrInterrupt,
	[PSXINT_CDREAD]	= cdrPlayReadInterrupt,
	[PSXINT_GPUDMA]	= gpuInterrupt,
	[PSXINT_MDECOUTDMA] = mdec1Interrupt,
	[PSXINT_SPUDMA]	= spuInterrupt,
	[PSXINT_MDECINDMA] = mdec0Interrupt,
	[PSXINT_GPUOTCDMA] = gpuotcInterrupt,
	[PSXINT_CDRDMA] = cdrDmaInterrupt,
	[PSXINT_NEWDRC_CHECK] = irqNoOp,
	[PSXINT_CDRLID] = cdrLidSeekInterrupt,
	[PSXINT_SPU_UPDATE] = spuUpdate,
	[PSXINT_SPU_IRQ] = spuDelayedIrq,
	[PSXINT_RCNT] = psxRcntUpdate,
	[PSXINT_LIGHTGUN] = lightgunInterrupt,
};

/* lightrec.c's dupe of psxBranchTest, using event_cycles */
void irq_test(psxCP0Regs *cp0)
{
	u32 cycle = psxRegs.cycle;
	u32 irq, irq_bits;

	for (irq = 0, irq_bits = psxRegs.interrupt; irq_bits != 0; irq++, irq_bits >>= 1) {
		if (!(irq_bits & 1))
			continue;
		if ((s32)(cycle - event_cycles[irq]) >= 0) {
			// note: irq_funcs() also modify psxRegs.interrupt
			psxRegs.interrupt &= ~(1u << irq);
			irq_funcs[irq]();
		}
	}

	cp0->n.Cause &= ~0x400;
	if (psxHu32(0x1070) & psxHu32(0x1074))
		cp0->n.Cause |= 0x400;
	if (((cp0->n.Cause | 1) & cp0->n.SR & 0x401) == 0x401)
		psxException(0, 0, cp0);
}

/* ---- The menu's flags ------------------------------------------------- */

bool needInitCpu = true;
bool executingBios = false;
bool executedBios = false;

#ifndef WOS_HAVE_GLES
/* GlesGpu/gpuPlugin.c defines these when it is built. */
BOOL bDisplayNotSet = TRUE;
BOOL canClearFrameBuf = FALSE;
#endif

void LoadingBar_showBar(float percent, const char *string) { (void)percent; (void)string; }
void pauseRemovalThread(void) {}
void continueRemovalThread(void) {}

/* ---- The CD plugin's configuration entry points (cdriso.c) ----------- */

long CDR__configure(void) { return 0; }
long CDR__test(void) { return 0; }
void CDR__about(void) {}
long CDR__setfilename(char *filename) { (void)filename; return 0; }

/* ---- libogc's clock: "ticks" are microseconds here ------------------- */

long long gettime(void)
{
	return (long long)wos_now_us();
}

unsigned int diff_usec(long long start, long long end)
{
	return (unsigned int)(end - start);
}

unsigned int diff_msec(long long start, long long end)
{
	return (unsigned int)((end - start) / 1000);
}

/* ---- newlib has memalign but no posix_memalign ----------------------- */

int posix_memalign(void **ptr, size_t align, size_t size)
{
	void *p = memalign(align, size);
	if (!p)
		return ENOMEM;
	*ptr = p;
	return 0;
}
