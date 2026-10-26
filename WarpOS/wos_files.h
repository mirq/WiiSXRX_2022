/*
 * wos_files.h -- the fileBrowser function pointers over AmigaDOS paths.
 */
#ifndef WOS_FILES_H
#define WOS_FILES_H

#include "../Gamecube/fileBrowser/fileBrowser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Point isoFile_*, saveFile_* and biosFile_* at stdio; memory cards go in
 * save_dir, the BIOS is looked up in bios_dir. */
void wos_files_install(const char *save_dir, const char *bios_dir);

/* Fills out size; 0 when the file exists. */
int  wos_file_open(fileBrowser_file *file);

/* dir + name, with a '/' only where AmigaDOS needs one ("DH0:" needs none). */
void wos_join_path(char *dst, unsigned size, const char *dir, const char *name);

#ifdef __cplusplus
}
#endif

#endif
