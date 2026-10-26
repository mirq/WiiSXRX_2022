/*
 * wos_main.c -- WiiStation for WarpOS: the program's entry point.
 */
#include <stdio.h>

#include "wos_sys.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (wos_sys_init() != 0) {
        wos_sys_shutdown();
        return 20;
    }
    printf("WiiStation for WarpOS\n");
    wos_log("startup ok, clock %lu us", (unsigned long)wos_now_us());
    wos_sys_shutdown();
    return 0;
}
