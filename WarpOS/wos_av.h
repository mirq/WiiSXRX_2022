/* AltiVec in WiiStation: on a G4 only (the build keeps 603e code
 * elsewhere), and through wos_av_call (an aligned stack). Vector code lives
 * in WarpOS/av_*.c, built with -mcpu=7400 -maltivec -mabi=altivec, and keeps
 * its vector constants in memory aligned at run time (WOS_AV_CONST): the
 * hunk loader aligns sections to 8 bytes only, so .rodata vectors load
 * the wrong 16 bytes. SetEnv WSX_ALTIVEC 0: never. */
#ifndef WOS_AV_H
#define WOS_AV_H
extern int wos_av;                      /* 1: AltiVec code may run */
void wos_av_init(void);
long wos_av_call(void *fn, ...);
/* 16-byte aligned scratch for vector constants, filled at init */
void *wos_av_alloc(unsigned bytes);
#endif
