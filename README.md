# DOOM — PlayStation 2

A native PlayStation 2 port of DOOM: full speed, with hardware (gsKit) video,
native SPU2 audio, and **authentic OPL/FM music** — the classic Doom soundtrack
the way an AdLib / Sound Blaster played it.

Specialised for the PS2, built from [doomgeneric](https://github.com/ozkl/doomgeneric)
(and through it [Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom)
and id Software's original DOOM source).

## Features

- **Native gsKit video** — Doom's 8-bit framebuffer is uploaded as a PSMT8
  texture + CLUT and the GS does the palette expansion and bilinear upscale in
  hardware. Full speed at 320×200.
- **480p progressive output** (`GS480P=1`) for component / YPbPr displays, plus
  the standard NTSC 640×448 interlaced mode.
- **Native audsrv audio** — sound effects mixed on the EE and streamed to the
  SPU2 (no SDL audio).
- **OPL / FM music (DBOPL)** — AdLib-style synthesis driven from the IWAD's
  GENMIDI lump, mixed into the SPU2 stream.
- **Controller input** via libpad (DualShock).
- **Embedded or runtime WAD** — optionally bakes in the shareware DOOM1.WAD;
  otherwise supply a WAD at runtime.

## Building

Everything builds in the official ps2dev toolchain through Docker:

```sh
./build.sh                                       # ps2/doomgeneric.elf (no WAD baked in)
./build.sh EMBED_WAD=1                            # also embed shareware DOOM1.WAD
./build.sh GSKIT_VIDEO=1 GS480P=1 EMBED_WAD=1     # native gsKit video, 480p output
```

(Or `cd ps2 && make …` inside the toolchain — see [`ps2/README.md`](ps2/README.md).)
Run the resulting ELF in PCSX2 or on real hardware.

## Controls

D-pad move / turn · **✕** fire · **○** use · **□** run · **L1/R1** strafe ·
**△** Enter · **Start** menu · **Select** automap

## WADs & copyright

No game data is committed to this repository. The shareware **DOOM1.WAD** (which
id Software permits redistributing) can be embedded for convenience; commercial
IWADs (DOOM.WAD, DOOM2.WAD) are never included — supply your own.

## Credits & licence

Released under the **GPLv2** (see [`LICENSE`](LICENSE)). This port stands on:

- [doomgeneric](https://github.com/ozkl/doomgeneric) by ozkl
- [Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom)
- id Software's original DOOM source
- the **DBOPL** OPL2/OPL3 emulator (from DOSBox)
- [ps2sdk](https://github.com/ps2dev/ps2sdk) and [gsKit](https://github.com/ps2dev/gsKit) (ps2dev)
