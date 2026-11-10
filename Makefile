# The Neverhood PC Native - build from MSYS2 MINGW64:  make -j16
#
# src/gen/nhc and src/gen/smk are nhc.exe and smackw32.dll recompiled to C
# by tools/recomp.py (make gen); src/rt is the PC runtime that replaces
# Windows 95, DirectDraw and DirectSound under them.
CC    = gcc
B     = build
PY   ?= /c/Users/Msi/AppData/Local/Programs/Python/Python310/python.exe
EXE   = Neverhood.exe

$(shell mkdir -p $(B)/tmp)
export TMP  := $(shell cygpath -m "$(CURDIR)/$(B)/tmp")
export TEMP := $(TMP)

SDL_CFLAGS := $(shell sdl2-config --cflags | sed 's/-Dmain=SDL_main//')
SDL_LIBS   := $(shell sdl2-config --static-libs | sed 's/-lSDL2main//;s/-mwindows//')

CFLAGS_GEN := -std=gnu11 -O2 -g0 -fwrapv -fno-strict-aliasing -w -Isrc/rt
CFLAGS_RT  := -DSDL_MAIN_HANDLED -std=gnu11 -O2 -g -Wall -Wno-unused-function -fwrapv -fno-strict-aliasing -Isrc/rt $(SDL_CFLAGS)
LIBS       := -static $(SDL_LIBS) -ld3d9 -lwinmm -lgdi32 -lm

GEN_SRCS := $(wildcard src/gen/*/*.c)
RT_SRCS  := $(wildcard src/rt/*.c)
OBJS := $(patsubst src/%.c,$(B)/%.o,$(GEN_SRCS) $(RT_SRCS))
# the dev build gets the game's icon from the local CD copy (never stored in the repo)
ICON_SRC := game/cd/nhood.ico
RES_OBJ := $(if $(wildcard $(ICON_SRC)),$(B)/game_res.o,)

all: $(EXE)

$(EXE): $(OBJS) $(RES_OBJ)
	$(CC) -mwindows -o $@ $^ $(LIBS)

$(B)/game_res.o: src/rt/game.rc $(ICON_SRC)
	cp $(ICON_SRC) $(B)/game.ico
	windres -I $(B) $< -o $@

$(B)/gen/%.o: src/gen/%.c src/rt/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_GEN) -I$(dir $<) -c $< -o $@

$(B)/rt/%.o: src/rt/%.c src/rt/rt.h src/rt/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_RT) -c $< -o $@

gen:
	$(PY) tools/recomp.py gamedata/nhc.exe re/nhc/functions.tsv tools/native_nhc.txt src/gen/nhc nhc
	$(PY) tools/recomp.py "gamedata/smackw32.dll@10000000@7f100000" re/smk/functions.tsv tools/native_smk.txt src/gen/smk smk

clean:
	rm -rf $(B)/gen $(B)/rt $(EXE)

.PHONY: all gen clean

# ---- release: game exe without the game's icon (setup puts it in from the
# user's CD) and the setup program with that exe inside ----
release: build/release/game.exe build/setup.exe
	cp build/setup.exe "The Neverhood PC Setup.exe"

build/release/game.exe: $(OBJS)
	@mkdir -p build/release
	$(CC) -mwindows -o $@ $^ $(LIBS)
	strip $@

SETUP_SRCS := $(wildcard installer/*.c)
SETUP_LIBS := -lcomctl32 -lcomdlg32 -lole32 -luuid -lshell32 -ladvapi32

$(B)/setup_res.o: installer/setup.rc installer/setup.manifest build/release/game.exe
	windres -I installer $< -o $@

build/setup.exe: $(SETUP_SRCS) installer/setup.h installer/install.h src/rt/game.h $(B)/setup_res.o
	$(CC) -O2 -Wall -Wno-format-truncation -mwindows -static -Isrc/rt -o $@ $(SETUP_SRCS) $(B)/setup_res.o $(SETUP_LIBS)

.PHONY: release
