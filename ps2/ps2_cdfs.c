// PS2 disc (ISO9660) access so a WAD can be read off the boot disc on demand.
// ps2_drivers loads cdfs.irx (+ CDVD) via init_cdfs_driver(). The WAD is read
// with the fio API (see w_file_cdfs.c) -- cdfs is a legacy ioman device that
// fopen can't reach and that needs FIO_O_RDONLY. Mirrors ps2quake on this SDK.

#include <stdio.h>

#include <ps2_cdfs_driver.h>   // init_cdfs_driver

void PS2Cdfs_Init(void)
{
    static int inited = 0;

    if (inited)
        return;
    inited = 1;

    printf("cdfs: init_cdfs_driver() = %d\n", (int) init_cdfs_driver());
    fflush(stdout);
}
