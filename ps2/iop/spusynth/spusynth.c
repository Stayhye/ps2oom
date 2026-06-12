// spusynth.irx -- PS2 SPU2 hardware synth (IOP side), libsd/sceSd version.
//
// Drives the SPU2's hardware ADPCM voices via libsd (sceSd*). Crucially it does
// NOT initialise the SPU2 itself: the EE side first brings up the proven audsrv
// stack (which powers + configures the SPU2 on real hardware and under PCSX2),
// and this module then keys extra voices on the now-live chip, sharing the same
// libsd.irx. That sidesteps the standalone bring-up that left a self-contained
// driver completely silent (and the IOP->SPU2 DMA timing out).
//
// MILESTONE S1: key one tone voice (square sample) + one noise voice, as a
// self-test, to confirm a hardware voice sounds before any RPC/MIDI is wired.

#include <irx.h>
#include <loadcore.h>      // MODULE_RESIDENT_END
#include <stdio.h>         // printf -> IOP console
#include <libsd.h>         // sceSd*, SD_*, ADPCM_LOOP_* (via libsd-common.h)

IRX_ID("spusynth", 1, 1);

// --- a hand-built PS-ADPCM square wave (see earlier notes) -----------------
#define NBLK     16
#define SMPBYTES (NBLK * 16)
#define SPU_ADDR 0x5000          // byte offset into the 2 MB SPU RAM

static u8 adpcm[SMPBYTES] __attribute__((aligned(64)));

static void build_square(void)
{
    int b, i;
    for (b = 0; b < NBLK; b++)
    {
        u8 *blk = adpcm + b * 16;
        blk[0] = 0x00;                                   // shift 0, filter 0
        blk[1] = (b == 0)        ? ADPCM_LOOP_START
               : (b == NBLK - 1) ? (ADPCM_LOOP_END | ADPCM_LOOP)
               :                    ADPCM_LOOP;
        for (i = 0; i < 7; i++) blk[2 + i] = 0x77;       // 14 samples = +7
        for (i = 0; i < 7; i++) blk[9 + i] = 0x88;       // 14 samples = -8
    }
}

int _start(int argc, char **argv)
{
    u32 spu = SPU_ADDR;
    u32 m1  = 1u << 1;   // tone  -> voice 1
    u32 m2  = 1u << 2;   // noise -> voice 2  (audsrv uses voice 0 + 22/23)

    (void) argc; (void) argv;
    printf("spusynth: start (sceSd, piggyback on audsrv)\n");

    build_square();

    // SPU2 is already initialised by audsrv; just (re)assert master volume and
    // the core mixer routing so our voices reach the output.
    sceSdSetParam(0 | SD_PARAM_MVOLL, 0x3FFF);
    sceSdSetParam(0 | SD_PARAM_MVOLR, 0x3FFF);
    sceSdSetParam(0 | SD_PARAM_MMIX,  0x00FF);
    sceSdSetParam(1 | SD_PARAM_MMIX,  0x00FF);

    // Upload the sample. IO (PIO) mode -- the IOP->SPU2 DMA path times out here.
    sceSdVoiceTrans(0, SD_TRANS_WRITE | SD_TRANS_MODE_IO, adpcm, &spu, SMPBYTES);
    sceSdVoiceTransStatus(0, 1);

    // Tone voice (1): full volume, ~1.5 kHz, fast attack, full sustain.
    sceSdSetParam(SD_VOICE(0, 1) | SD_VPARAM_VOLL,  0x3FFF);
    sceSdSetParam(SD_VOICE(0, 1) | SD_VPARAM_VOLR,  0x3FFF);
    sceSdSetParam(SD_VOICE(0, 1) | SD_VPARAM_PITCH, 0x1000);
    sceSdSetParam(SD_VOICE(0, 1) | SD_VPARAM_ADSR1, SD_SET_ADSR1(0, 0x08, 0x0, 0xF));
    sceSdSetParam(SD_VOICE(0, 1) | SD_VPARAM_ADSR2, SD_SET_ADSR2(0, 0x00, 0, 0x00));
    sceSdSetAddr (SD_VOICE(0, 1) | SD_VADDR_SSA,    SPU_ADDR);

    // Noise voice (2): diagnostic -- needs no sample. NON routes the noise
    // source into this voice instead of ADPCM playback.
    sceSdSetParam(SD_VOICE(0, 2) | SD_VPARAM_VOLL,  0x3FFF);
    sceSdSetParam(SD_VOICE(0, 2) | SD_VPARAM_VOLR,  0x3FFF);
    sceSdSetParam(SD_VOICE(0, 2) | SD_VPARAM_ADSR1, SD_SET_ADSR1(0, 0x08, 0x0, 0xF));
    sceSdSetParam(SD_VOICE(0, 2) | SD_VPARAM_ADSR2, SD_SET_ADSR2(0, 0x00, 0, 0x00));
    sceSdSetSwitch(SD_SWITCH_NON | 0, m2);

    // Route both voices into the L/R mix, then key both on.
    sceSdSetSwitch(SD_SWITCH_VMIXL | 0, m1 | m2);
    sceSdSetSwitch(SD_SWITCH_VMIXR | 0, m1 | m2);
    sceSdSetSwitch(SD_SWITCH_KON   | 0, m1 | m2);

    printf("spusynth: voices keyed (tone v1 @ spu 0x%x + noise v2)\n", SPU_ADDR);
    return MODULE_RESIDENT_END;
}
