// PS2 memory-card persistence for the controller settings.
//
// FAILSAFE BY DESIGN: if no (formatted PS2) memory card is in slot 0 -- or any
// libmc step fails -- we simply do nothing and the settings live in RAM for the
// session (their config-file / default values). Nothing here can crash or hang
// the game when a card is absent; every step is checked and bails out quietly.
//
//   boot:           PS2Mc_LoadControls()  overlays the saved values (if any)
//   quit-to-menu:   PS2Mc_SaveControls()  writes them back (from I_Quit)
//
// The blob is stored as a single loose file in the card root ("/PS2OOM.CFG"),
// not a save folder -- so it stays invisible in the PS2 BIOS save browser (which
// only lists directories) instead of showing up as "corrupted data".

#include <tamtypes.h>
#include <loadfile.h>   // SifLoadModule
#include <libmc.h>      // mcInit/mcGetInfo/mcOpen/mcRead/mcWrite/mcClose/mcFlush/mcSync
#include <string.h>
#include <stdio.h>

// mcOpen() mode flags (classic PS2 IOP fileio values; the SDK's fcntl.h has no
// O_* macros). create == sceMcFileCreateFile (0x0200).
#define MC_O_RDONLY 0x0001
#define MC_O_WRONLY 0x0002
#define MC_O_CREAT  0x0200

#define MC_PORT     0
#define MC_SLOT     0
#define MC_CFG_FILE "/PS2OOM.CFG"
#define MC_MAGIC    0x324F4F4D    // 'M','O','O','2'

// Controller settings, owned by ps2/ps2_pad.c and bound to the config file.
extern int ps2_turn_sensitivity;
extern int ps2_always_run;
extern int ps2_stick_deadzone;
extern int ps2_invert_y;
extern int ps2_southpaw;

typedef struct
{
    u32 magic;
    s32 version;
    s32 turn_sensitivity;
    s32 always_run;
    s32 stick_deadzone;
    s32 invert_y;
    s32 southpaw;
    s32 reserved[9];
} mc_blob_t;   // 64 bytes

static int g_state = 0;   // 0 = not probed, 1 = card ready, -1 = unavailable

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Lazy one-time probe: load the IOP modules, init libmc, and confirm a
// formatted PS2 card is present in slot 0. Returns 1 if usable, else 0.
static int mc_ready(void)
{
    int type = 0, freeclu = 0, format = 0, r = -1;

    if (g_state != 0)
        return g_state == 1;

    // 1) IOP modules (SIO2MAN is usually already up via the pad; harmless to
    //    request again). libmc auto-detects MCMAN/MCSERV vs XMCMAN/XMCSERV.
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:MCMAN",   0, NULL);
    SifLoadModule("rom0:MCSERV",  0, NULL);

    // 2) init the library.
    if (mcInit(MC_TYPE_MC) < 0)
    {
        printf("mc: mcInit failed -- controller settings stay in RAM\n");
        g_state = -1;
        return 0;
    }

    // 3) formatted PS2 card in slot 0?  r: 0 = ok, -1 = card replaced (still
    //    present); anything lower = no card / error.
    mcGetInfo(MC_PORT, MC_SLOT, &type, &freeclu, &format);
    mcSync(MC_WAIT, NULL, &r);
    if (r < -1 || type != MC_TYPE_PS2 || format != MC_FORMATTED)
    {
        printf("mc: no formatted card in slot 0 (r=%d type=%d fmt=%d) -- RAM only\n",
               r, type, format);
        g_state = -1;
        return 0;
    }

    printf("mc: memory card ready (%d free clusters)\n", freeclu);
    g_state = 1;
    return 1;
}

// Overlay saved controller settings onto the live globals. No card / no file /
// bad data -> leave the existing (default or config-file) values untouched.
void PS2Mc_LoadControls(void)
{
    mc_blob_t b;
    int fd = -1, n = -1, r = -1;

    if (!mc_ready())
        return;

    mcOpen(MC_PORT, MC_SLOT, MC_CFG_FILE, MC_O_RDONLY);
    mcSync(MC_WAIT, NULL, &fd);
    if (fd < 0)
    {
        printf("mc: no saved settings yet (first run)\n");
        return;
    }

    mcRead(fd, &b, sizeof(b));
    mcSync(MC_WAIT, NULL, &n);
    mcClose(fd);
    mcSync(MC_WAIT, NULL, &r);

    if (n != (int)sizeof(b) || b.magic != MC_MAGIC)
    {
        printf("mc: settings file invalid (n=%d) -- ignoring\n", n);
        return;
    }

    // Clamp to valid ranges so a corrupt/old file can't wedge the controls.
    ps2_turn_sensitivity = clampi(b.turn_sensitivity, 1, 20);
    ps2_always_run       = b.always_run ? 1 : 0;
    ps2_stick_deadzone   = clampi(b.stick_deadzone, 10, 90);
    ps2_invert_y         = b.invert_y ? 1 : 0;
    ps2_southpaw         = b.southpaw ? 1 : 0;
    printf("mc: loaded controller settings\n");
}

// Write the live controller settings to the card. No-op when no card.
void PS2Mc_SaveControls(void)
{
    mc_blob_t b;
    int fd = -1, n = -1, r = -1;

    if (!mc_ready())
        return;

    memset(&b, 0, sizeof(b));
    b.magic            = MC_MAGIC;
    b.version          = 1;
    b.turn_sensitivity = ps2_turn_sensitivity;
    b.always_run       = ps2_always_run;
    b.stick_deadzone   = ps2_stick_deadzone;
    b.invert_y         = ps2_invert_y;
    b.southpaw         = ps2_southpaw;

    mcOpen(MC_PORT, MC_SLOT, MC_CFG_FILE, MC_O_CREAT | MC_O_WRONLY);
    mcSync(MC_WAIT, NULL, &fd);
    if (fd < 0)
    {
        printf("mc: open-for-write failed (%d)\n", fd);
        return;
    }

    mcWrite(fd, &b, sizeof(b));
    mcSync(MC_WAIT, NULL, &n);
    mcFlush(fd);
    mcSync(MC_WAIT, NULL, &r);
    mcClose(fd);
    mcSync(MC_WAIT, NULL, &r);
    printf("mc: saved controller settings (%d bytes)\n", n);
}
