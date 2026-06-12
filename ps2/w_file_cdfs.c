// WAD I/O backend for the disc (ISO9660 via cdfs.irx).
//
// cdfs.irx is a LEGACY ioman fio driver: POSIX fopen can't reach it, AND it
// rejects newlib's O_RDONLY (0) -- it requires FIO_O_RDONLY (1). So the WAD is
// read on demand with the fio API (as ps2quake does on this same toolchain).
// Using fio in the newlib port needs NEWLIB_PORT_AWARE to silence the SDK guard.
// On-demand reads let a 27 MB WAD stream off the disc (it can't fit in 32 MB RAM).

#define NEWLIB_PORT_AWARE       // we deliberately use fio for the cdfs (ioman) device

#include <stdlib.h>
#include <fileio.h>             // fioOpen/fioClose/fioRead/fioLseek, FIO_O_RDONLY, SEEK_*

#include "doomtype.h"
#include "w_file.h"

extern wad_file_class_t cdfs_wad_file;

typedef struct
{
    wad_file_t wad;
    int        fd;
} cdfs_wad_file_t;

// 1 if the cdfs file can be opened, else 0. (fopen would fail: O_RDONLY == 0.)
int PS2Cdfs_Exists(const char *path)
{
    int fd = fioOpen((char *) path, FIO_O_RDONLY);
    if (fd < 0)
        return 0;
    fioClose(fd);
    return 1;
}

static wad_file_t *CDFS_OpenFile(char *path)
{
    cdfs_wad_file_t *result;
    int              fd;

    fd = fioOpen(path, FIO_O_RDONLY);
    if (fd < 0)
        return NULL;

    result = malloc(sizeof(cdfs_wad_file_t));
    if (result == NULL)
    {
        fioClose(fd);
        return NULL;
    }
    result->wad.file_class = &cdfs_wad_file;
    result->wad.mapped     = NULL;
    result->wad.length     = (unsigned int) fioLseek(fd, 0, SEEK_END);
    result->fd             = fd;
    return &result->wad;
}

static void CDFS_CloseFile(wad_file_t *wad)
{
    cdfs_wad_file_t *cdfs = (cdfs_wad_file_t *) wad;
    fioClose(cdfs->fd);
    free(cdfs);
}

static size_t CDFS_Read(wad_file_t *wad, unsigned int offset,
                        void *buffer, size_t buffer_len)
{
    cdfs_wad_file_t *cdfs = (cdfs_wad_file_t *) wad;
    int              n;

    fioLseek(cdfs->fd, (int) offset, SEEK_SET);
    n = fioRead(cdfs->fd, buffer, (int) buffer_len);
    return (n < 0) ? 0 : (size_t) n;
}

wad_file_class_t cdfs_wad_file =
{
    CDFS_OpenFile,
    CDFS_CloseFile,
    CDFS_Read,
};
