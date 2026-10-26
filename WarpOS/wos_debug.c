/*
 * wos_debug.c -- Gamecube/DEBUG.h's interface for the WarpOS build: every
 * line goes to the buffered log (wos_log), none to the screen.
 */
#include <stdio.h>
#include <string.h>

#include "../Gamecube/DEBUG.h"
#include "wos_sys.h"

char text[DEBUG_TEXT_HEIGHT][DEBUG_TEXT_WIDTH];
char txtbuffer[1024];
char printToSD = 0;

void DEBUG_print(char *string, int pos)
{
    (void)pos;
    if (string && *string)
        wos_log("%s", string);
}

void DEBUG_stats(int stats_id, char *info, unsigned int stats_type, unsigned int adjustment_value)
{
    (void)stats_id; (void)info; (void)stats_type; (void)adjustment_value;
}

void DEBUG_update(void) {}

char **DEBUG_get_text(void)
{
    return (char **)text;
}

void openLogFile(void) {}
void closeLogFile(void) { wos_log_flush(); }

void writeLogFile(char *string)
{
    if (string && *string)
        wos_log("%s", string);
}

void printFunctionName(void) {}
