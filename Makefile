# Wolverine's Revenge static recompilation
GAME_DIR ?= game
GAME_EXE := $(GAME_DIR)/Wolvie.exe
OUT      := build/wolvie
GEN      := $(OUT)/gen
PY       := .venv/bin/python

CC       := clang
OPT      ?= -O1
CFLAGS   := $(OPT) -g -fno-strict-aliasing -fwrapv -w -Iruntime -I$(GEN) -I/opt/homebrew/include
LDFLAGS  := -Wl,-export_dynamic -L/opt/homebrew/lib -lSDL3 -lavformat -lavcodec -lswscale -lswresample -lavutil -framework Metal -framework QuartzCore -framework CoreGraphics -framework Foundation -framework GameController

RT_SRC   := $(wildcard runtime/*.c runtime/win32/*.c runtime/d3d8/*.c)
RT_OBJC  := $(wildcard runtime/*.m runtime/d3d8/*.m)
GEN_SRC   = $(wildcard $(GEN)/*.c)
RT_OBJ   := $(patsubst %.c,$(OUT)/obj/%.o,$(RT_SRC)) $(patsubst %.m,$(OUT)/obj/%.o,$(RT_OBJC))
GEN_OBJ   = $(patsubst $(GEN)/%.c,$(OUT)/obj/gen/%.o,$(GEN_SRC))

.PHONY: all recomp run clean link
all: $(OUT)/wolvie

recomp: $(GEN)/funcs.h
$(GEN)/funcs.h: recomp/*.py
	$(PY) recomp/main.py "$(GAME_EXE)" $(OUT)

$(OUT)/wolvie: $(GEN)/funcs.h FORCE
	@$(MAKE) --no-print-directory link

link: $(RT_OBJ) $(GEN_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(OUT)/wolvie $^

$(OUT)/obj/%.o: %.c runtime/*.h runtime/*/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/obj/%.o: %.m runtime/*.h runtime/d3d8/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fobjc-arc -c $< -o $@

$(OUT)/obj/gen/%.o: $(GEN)/%.c runtime/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

run: all
	cd "$(dir $(GAME_EXE))" && $(CURDIR)/$(OUT)/wolvie $(CURDIR)/$(OUT)/image.bin .

clean:
	rm -rf $(OUT)

FORCE:
