// PS2 SPU2 hardware synth -- EE side.
//
// Brings up the proven audsrv sound stack (which loads libsd.irx + audsrv.irx
// and initialises the SPU2), then loads our spusynth.irx. spusynth keys extra
// hardware voices on the now-live SPU2 via sceSd, sharing that libsd.irx -- a
// self-contained driver wouldn't power the chip up under emulation, but audsrv
// does. Later, the same coexistence lets synth music share the chip with SFX.

#include <stdio.h>
#include <loadfile.h>          // SifExecModuleBuffer
#include <ps2_audio_driver.h>  // init_audio_driver

// Embedded IRX module (bin2c).
extern unsigned char spusynth_irx[];  extern unsigned int size_spusynth_irx;

// S1: bring up audsrv (initialises the SPU2 + enables IRX-from-buffer loading),
// then load spusynth so the IRX keys a tone + noise voice. Caller should halt.
void PS2Spu_BeepTest(void)
{
    int ret = 0;

    // Initialise the SPU2 via the audsrv stack (also applies the lmb patches
    // we need to load our own IRX from an EE buffer below).
    init_audio_driver();

    if (SifExecModuleBuffer(spusynth_irx, size_spusynth_irx, 0, NULL, &ret) < 0)
        printf("spu: SPUSYNTH.IRX load FAILED\n");
    else
        printf("spu: SPUSYNTH.IRX loaded (start ret=%d) -- listen for a tone\n", ret);
}
