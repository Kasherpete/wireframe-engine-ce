# ----------------------------
# Makefile Options
# ----------------------------

NAME = DEMOPRGM
#ICON = icon.png
DESCRIPTION = "A Fast Wireframe Engine for the TI84+ CE"
COMPRESSED = YES

COMPRESSED_MODE = zx7
ARCHIVED = NO

CFLAGS = -Wall -Wextra -O3 -ffast-math
CXXFLAGS = -Wall -Wextra -O3 -ffast-math

# The eZ80 has no executable-stack concept, so the linker's missing .note.GNU-stack warnings
# (from CEdev's own crt0.o and libraries) are meaningless here.
EXTRA_PRE_LDFLAGS = --no-warn-execstack
EXTRA_LDFLAGS = --no-warn-execstack

# "make hud" (or "make debug hud") adds the on-screen HUD: FPS, timings and the buffer number.
# Plain "make" leaves it out, along with its per-frame cost (about 1.2 ms of fontlib drawing).
ifneq ($(filter hud,$(MAKECMDGOALS)),)
CFLAGS += -DSHOW_HUD=1
endif

# Objects do not depend on compiler flags, so switching between make, make debug and make hud
# would link stale objects. obj/variant changes whenever the variant does, forcing a rebuild.
VARIANT := $(or $(sort $(filter debug hud,$(MAKECMDGOALS))),release)
ifeq ($(filter clean gfx version,$(MAKECMDGOALS)),)
$(shell mkdir -p obj && (test "$$(cat obj/variant 2>/dev/null)" = "$(VARIANT)" || echo "$(VARIANT)" > obj/variant))
DEPS = obj/variant
endif

# "make" also builds the test world, bin/WORLD.8xv (utils/make_world.py), which the program loads
.DEFAULT_GOAL := default

# ----------------------------

include $(shell cedev-config --makefile)

.PHONY: default hud world
default: $(BUILD) world
hud: $(BUILD) world

world: $(BINDIR)/WORLD.8xv
$(BINDIR)/WORLD.8xv: utils/make_world.py
	$(Q)mkdir -p $(BINDIR)
	$(Q)python3 utils/make_world.py $@
