// spusynth.irx -- PS2 SPU2 hardware synth (IOP side), libsd/sceSd version.
//
// Drives the SPU2's hardware ADPCM voices via libsd (sceSd*). Crucially it does
// NOT initialise the SPU2 itself: the EE side first brings up the proven audsrv
// stack (which powers + configures the SPU2 on real hardware and under PCSX2),
// and this module then keys voices on the now-live chip, sharing the same
// libsd.irx.
//
// MILESTONE S1 (proven): one tone voice sounds end-to-end. The two fixes were
// to key on CORE 1 (audsrv zeroes core 0's master volume) and to route the
// voice-dry MMIX gates (SndL|SndR = 0xC00); see [[spu2-synth-routing]].
//
// MILESTONE S2 (this file): polyphony + real note pitches + ADSR. A pool of
// core-1 voices share one looping square-wave sample; each is pitched from a
// MIDI note via sceSdNote2Pitch, gated by an attack/release envelope, and
// keyed on/off by a sequencer thread. That thread is the model the S3 MIDI
// player will reuse -- _start returns RESIDENT and the music plays on the IOP.

#include <irx.h>
#include <loadcore.h>      // MODULE_RESIDENT_END
#include <stdio.h>         // printf -> IOP console
#include <thbase.h>        // CreateThread/StartThread/DelayThread (sequencer)
#include <libsd.h>         // sceSd*, SD_*, ADPCM_LOOP_* (via libsd-common.h)

IRX_ID("spusynth", 1, 2);

// --- SPU2 routing (see S1 notes / [[spu2-synth-routing]]) ------------------
#define CORE         1          // audsrv's live DAC core
#define MMIX_SND_DRY 0xC00      // DryGate.SndL|SndR: route voices to L/R output

// --- voice pool ------------------------------------------------------------
#define FIRST_VOICE  1          // voice 0 left alone; audsrv streams via Ext
#define NVOICES      4          // 4-note polyphony for the self-test
#define VOICE_MASK   (((1u << NVOICES) - 1u) << FIRST_VOICE)  // bits 1..4

// --- the shared looping square-wave sample ---------------------------------
// NBLK large on purpose: at ~1575 Hz unity a block is 28 samples, so 1024
// blocks is ~0.65 s of solid square even if the loop flags are ignored. That
// makes this a clean test of whether silence is a *looping* failure (short
// sample played once = inaudible blip, with the ADSR envelope still reading
// full) versus an output-path issue.
#define NBLK     1024
#define SMPBYTES (NBLK * 16)
// 0x5000 reads back fine (uploads to ~1 MB silently fail); keep the sample low.
#define SPU_ADDR 0x5000          // byte offset into the 2 MB SPU RAM

// The square's loop is 28 samples -> ~1714 Hz at unity pitch (0x1000), which
// is ~MIDI note 92.5. Telling sceSdNote2Pitch that centre makes requested MIDI
// notes come out at the right musical frequency.
#define BASE_NOTE 92
#define BASE_FINE 64             // +0.5 semitone (fine is 0..127 over a semitone)

static u8 adpcm[SMPBYTES] __attribute__((aligned(64)));

static int g_diag = 1;   // log the first arpeggio pass, then go quiet

static void build_square(u8 hdr)
{
    int b, i;
    for (b = 0; b < NBLK; b++)
    {
        u8 *blk = adpcm + b * 16;
        blk[0] = hdr;                                    // shift/filter under test
        blk[1] = (b == 0)        ? ADPCM_LOOP_START
               : (b == NBLK - 1) ? (ADPCM_LOOP_END | ADPCM_LOOP)
               :                    ADPCM_LOOP;
        for (i = 0; i < 7; i++) blk[2 + i] = 0x77;       // 14 samples = +7
        for (i = 0; i < 7; i++) blk[9 + i] = 0x88;       // 14 samples = -8
    }
}

// Configure a voice for a MIDI note and key it on. VMIX (the per-voice route
// into the core mix) is set once globally in _start, so note_on only has to
// (re)point the voice, pitch it, and trigger key-on.
static void note_on(int voice, int midinote, int vol)
{
    int e = SD_VOICE(CORE, voice);
    u16 pitch = sceSdNote2Pitch(BASE_NOTE, BASE_FINE, (u16) midinote, 0);

    // Per-voice volume must leave mix headroom: the square is full-scale, so
    // four at 0x3FFF overdrive the core mix into clipping ("high-pitch noise").
    sceSdSetParam(e | SD_VPARAM_VOLL,  (u16) vol);
    sceSdSetParam(e | SD_VPARAM_VOLR,  (u16) vol);
    sceSdSetParam(e | SD_VPARAM_PITCH, pitch);
    // Instant attack, no decay, full sustain; gentle linear release on key-off.
    sceSdSetParam(e | SD_VPARAM_ADSR1, SD_SET_ADSR1(SD_ADSR_AR_EXPi, 0x00, 0x00, 0x0F));
    sceSdSetParam(e | SD_VPARAM_ADSR2, SD_SET_ADSR2(SD_ADSR_SR_EXPd, 0x7F, SD_ADSR_RR_LINEARd, 0x0C));
    sceSdSetAddr (e | SD_VADDR_SSA,    SPU_ADDR);

    if (g_diag)
        printf("spusynth: note_on v%d note=%d pitch=0x%x\n", voice, midinote, pitch);

    sceSdSetSwitch(SD_SWITCH_KON | CORE, 1u << voice);   // trigger: doesn't clobber others
}

static void note_off(int voice)
{
    sceSdSetSwitch(SD_SWITCH_KOFF | CORE, 1u << voice);  // -> ADSR release stage
}

// Upload the shared sample to SPU RAM. NOTE: libsd's sceSdVoiceTrans uses the
// *pointer value* of the spuaddr argument as the destination byte address (it
// does not dereference it), and the IO path casts it to u16 -- so pass the
// address itself, cast to a pointer, and keep it within 64 KB.
static void upload_sample(void)
{
    sceSdVoiceTrans(0, SD_TRANS_WRITE | SD_TRANS_MODE_IO,
                    adpcm, (u32 *) SPU_ADDR, SMPBYTES);
    sceSdVoiceTransStatus(0, 1);
}

// Sequencer demo, looping:
//   1. a monophonic C-major scale on one voice (tuning + attack/release), then
//   2. the full C-major chord across the voice pool at reduced per-voice
//      volume (polyphony, without clipping).
static void seq_thread(void *arg)
{
    static const int scale[8] = { 60, 62, 64, 65, 67, 69, 71, 72 };  // C major
    static const int chord[NVOICES] = { 60, 64, 67, 72 };            // C E G C
    int i;

    (void) arg;
    printf("spusynth: seq thread entered\n");

    for (;;)
    {
        // 1) Clean monophonic scale on voice FIRST_VOICE.
        for (i = 0; i < 8; i++)
        {
            note_on(FIRST_VOICE, scale[i], 0x3FFF);
            DelayThread(260000);
            note_off(FIRST_VOICE);
            DelayThread(50000);
        }

        DelayThread(350000);

        // 2) Full chord, per-voice ~0x3FFF/4 for mix headroom.
        for (i = 0; i < NVOICES; i++)
            note_on(FIRST_VOICE + i, chord[i], 0x1000);

        DelayThread(1100000);                    // let it ring
        for (i = 0; i < NVOICES; i++)            // release: hear the tails fade
            note_off(FIRST_VOICE + i);

        g_diag = 0;                              // logged one full pass; quiet now
        DelayThread(900000);
    }
}

int _start(int argc, char **argv)
{
    u16 mmix;
    iop_thread_t th;
    int tid;

    (void) argc; (void) argv;
    printf("spusynth: start (S2 polyphony, core %d, %d voices)\n", CORE, NVOICES);

    build_square(0x00);

    // SPU2 is already up (audsrv). Re-assert core 1 master volume and OR the
    // voice-dry gates into its mixer without disturbing audsrv's Ext routing.
    // SPU2 volume in fixed mode is a 15-bit SIGNED field: bit 14 is the sign,
    // so 0x3FFF is the max positive volume. 0x7FFF sets bit 14 -> negative ->
    // silence. 0x3FFF is the ceiling for both master and voice.
    sceSdSetParam(CORE | SD_PARAM_MVOLL, 0x3FFF);
    sceSdSetParam(CORE | SD_PARAM_MVOLR, 0x3FFF);
    mmix = sceSdGetParam(CORE | SD_PARAM_MMIX);
    sceSdSetParam(CORE | SD_PARAM_MMIX, mmix | MMIX_SND_DRY);

    // Upload the shared sample once (the address fix lives in upload_sample).
    upload_sample();

    // Route the whole voice pool into the L/R mix once; envelopes (not VMIX)
    // gate when each voice is actually heard.
    sceSdSetSwitch(SD_SWITCH_VMIXL | CORE, VOICE_MASK);
    sceSdSetSwitch(SD_SWITCH_VMIXR | CORE, VOICE_MASK);

    // Kick off the sequencer on its own IOP thread so _start can go resident.
    th.attr      = TH_C;
    th.option    = 0;
    th.thread    = seq_thread;
    th.stacksize = 0x1000;
    th.priority  = 0x40;
    tid = CreateThread(&th);
    if (tid > 0)
        StartThread(tid, NULL);
    else
        printf("spusynth: CreateThread failed (%d)\n", tid);

    printf("spusynth: sequencer tid=%d (voices 0x%x @ spu 0x%x)\n", tid, VOICE_MASK, SPU_ADDR);
    return MODULE_RESIDENT_END;
}
