/* AltiVec detection for WiiStation (see wos_av.h). */
#include <stdlib.h>
#include <string.h>
#include "wos_amiga.h"
#include "wos_sys.h"
#include "wos_av.h"

extern int wos_env_int(const char *name, int def);

int wos_av;

void wos_av_init(void)
{
    struct TagItem t[2] = { { GETINFO_CPU, 0 }, { TAG_END, 0 } };
    GetInfo(t);
    wos_av = (t[0].ti_Data & (CPUF_G4 | CPUF_7410 | CPUF_7441)) && wos_env_int("WSX_ALTIVEC", 1);
    wos_log("[av] cpu flags %08lx: AltiVec %s", (unsigned long)t[0].ti_Data,
            wos_av ? "on" : "off");
}

void *wos_av_alloc(unsigned bytes)
{
    unsigned char *p = calloc(1, bytes + 16);
    return p ? (void *)(((unsigned long)p + 15) & ~15ul) : NULL;
}
