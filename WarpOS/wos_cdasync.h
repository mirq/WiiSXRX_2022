/*
 * wos_cdasync.h -- disc image reads that run while the emulator does.
 */
#ifndef WOS_CDASYNC_H
#define WOS_CDASYNC_H

int  wos_cda_open(const char *path);   /* 1 when it can serve the image */
void wos_cda_close(void);
int  wos_cda_ok(void);
/* One raw 2352-byte sector into dest; 2352, or -1. */
int  wos_cda_read(int sector, void *dest);

#endif
