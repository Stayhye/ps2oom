# spusynth.irx -- SPU2 hardware synth IOP module.
# Built with the IOP toolchain (mipsel-none-elf-) via the SDK's IRX rules.
# Uses libsd (sceSd*) imported from the libsd.irx that the audsrv stack loads;
# it does not init the SPU2 itself (audsrv does), so nothing extra to link.

IOP_BIN  = spusynth.irx
IOP_OBJS = spusynth.o imports.o

IOP_CFLAGS += -Wall

all: $(IOP_BIN)

clean:
	rm -f $(IOP_BIN) $(IOP_OBJS_DIR)*.o build-imports.c

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
