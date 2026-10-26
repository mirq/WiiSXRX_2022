/*
 * wos_files.c -- the fileBrowser interface (Gamecube/fileBrowser/fileBrowser.h)
 * over newlib stdio, for the WarpOS build.
 *
 * The Wii build reaches its SD card, USB stick, DVD and SMB shares through
 * these function pointers; here every one of them is a plain AmigaDOS path.
 * The ISO handles stay open between reads (one per kind: image, CDDA, SUB),
 * as fileBrowser-libfat.c's ROM functions do.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../Gamecube/fileBrowser/fileBrowser.h"
#include "wos_files.h"

#define MAX_FILE_PTRS 3
static FILE *s_rom_fd[MAX_FILE_PTRS];

static int wos_file_init(fileBrowser_file *f)
{
    (void)f;
    return 1;               /* "device present" */
}

static int wos_file_deinit(fileBrowser_file *f)
{
    (void)f;
    return 0;
}

int wos_file_open(fileBrowser_file *file)
{
    struct stat st;
    if (!stat(file->name, &st)) {
        file->offset = 0;
        file->discoffset = 0;
        file->size = st.st_size;
        return 0;
    }
    return FILE_BROWSER_ERROR_NO_FILE;
}

static int wos_file_seek(fileBrowser_file *file, unsigned int where, unsigned int type)
{
    if (type == FILE_BROWSER_SEEK_SET)      file->offset = where;
    else if (type == FILE_BROWSER_SEEK_CUR) file->offset += where;
    else                                    file->offset = file->size + where;
    return 0;
}

static int wos_file_read(fileBrowser_file *file, void *buffer, unsigned int length)
{
    FILE *f = fopen(file->name, "rb");
    int n;
    if (!f)
        return FILE_BROWSER_ERROR;
    fseek(f, file->offset, SEEK_SET);
    n = (int)fread(buffer, 1, length, f);
    if (n > 0)
        file->offset += n;
    fclose(f);
    return n;
}

static int wos_file_write(fileBrowser_file *file, void *buffer, unsigned int length)
{
    FILE *f = fopen(file->name, file->offset ? "r+b" : "wb");
    int n;
    if (!f)
        return FILE_BROWSER_ERROR;
    fseek(f, file->offset, SEEK_SET);
    n = (int)fwrite(buffer, 1, length, f);
    if (n > 0)
        file->offset += n;
    fclose(f);
    return n;
}

static int wos_rom_deinit(fileBrowser_file *f)
{
    unsigned k = f->attr % MAX_FILE_PTRS;
    if (s_rom_fd[k])
        fclose(s_rom_fd[k]);
    s_rom_fd[k] = NULL;
    return 0;
}

static int wos_rom_read(fileBrowser_file *file, void *buffer, unsigned int length)
{
    unsigned k = file->attr % MAX_FILE_PTRS;
    int n;
    if (!s_rom_fd[k]) {
        s_rom_fd[k] = fopen(file->name, "rb");
        if (!s_rom_fd[k])
            return FILE_BROWSER_ERROR;
        setvbuf(s_rom_fd[k], NULL, _IOFBF, 16 * 1024);
    }
    fseek(s_rom_fd[k], file->offset, SEEK_SET);
    n = (int)fread(buffer, 1, length, s_rom_fd[k]);
    if (n > 0)
        file->offset += n;
    return n;
}

static fileBrowser_file s_save_dir;
static fileBrowser_file s_bios_dir;

void wos_files_install(const char *save_dir, const char *bios_dir)
{
    memset(&s_save_dir, 0, sizeof(s_save_dir));
    strncpy(s_save_dir.name, save_dir, FILE_BROWSER_MAX_PATH_LEN - 1);
    s_save_dir.attr = FILE_BROWSER_ATTR_DIR;
    memset(&s_bios_dir, 0, sizeof(s_bios_dir));
    strncpy(s_bios_dir.name, bios_dir, FILE_BROWSER_MAX_PATH_LEN - 1);
    s_bios_dir.attr = FILE_BROWSER_ATTR_DIR;

    isoFile_init     = wos_file_init;
    isoFile_open     = wos_file_open;
    isoFile_readDir  = NULL;
    isoFile_readFile = wos_rom_read;
    isoFile_seekFile = wos_file_seek;
    isoFile_deinit   = wos_rom_deinit;

    saveFile_dir       = &s_save_dir;
    saveFile_init      = wos_file_init;
    saveFile_readFile  = wos_file_read;
    saveFile_writeFile = wos_file_write;
    saveFile_deinit    = wos_file_deinit;

    biosFile_dir      = &s_bios_dir;
    biosFile_init     = wos_file_init;
    biosFile_open     = wos_file_open;
    biosFile_readFile = wos_file_read;
    biosFile_deinit   = wos_file_deinit;
}

void wos_join_path(char *dst, unsigned size, const char *dir, const char *name)
{
    unsigned n;
    if (!dir || !*dir) {
        strncpy(dst, name, size - 1);
        dst[size - 1] = 0;
        return;
    }
    strncpy(dst, dir, size - 1);
    dst[size - 1] = 0;
    n = strlen(dst);
    /* AmigaDOS: "DEV:" and "dir/" need no separator. */
    if (n && dst[n - 1] != ':' && dst[n - 1] != '/' && n + 1 < size) {
        dst[n++] = '/';
        dst[n] = 0;
    }
    strncat(dst, name, size - 1 - n);
}
