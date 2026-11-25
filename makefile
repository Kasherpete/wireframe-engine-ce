# ----------------------------
# Makefile Options
# ----------------------------

NAME = CALCRAFT
# the shell icon (16 x 16): icons/grass.png, icons/ore.png or icons/wood.png
ICON = icons/grass.png
DESCRIPTION = "Minecraft for the TI84+ CE!"
COMPRESSED = YES

COMPRESSED_MODE = zx7
ARCHIVED = NO

CFLAGS = -Wall -Wextra -O3 -ffast-math
CXXFLAGS = -Wall -Wextra -O3 -ffast-math

# The eZ80 has no executable-stack concept, so the linker's missing .note.GNU-stack warnings
# (from CEdev's own crt0.o and libraries) are meaningless here.
EXTRA_PRE_LDFLAGS = --no-warn-execstack
EXTRA_LDFLAGS = --no-warn-execstack

# Objects do not depend on compiler flags, so switching between make and make debug would link
# stale objects. obj/variant changes whenever the variant does, forcing a rebuild.
VARIANT := $(or $(filter debug,$(MAKECMDGOALS)),release)
ifeq ($(filter clean gfx version,$(MAKECMDGOALS)),)
$(shell mkdir -p obj && (test "$$(cat obj/variant 2>/dev/null)" = "$(VARIANT)" || echo "$(VARIANT)" > obj/variant))
DEPS = obj/variant
endif

# "make" also builds the test world, bin/WORLD.8xv (utils/make_world.py), which the program loads,
# and the large tutorial-style worlds (bin/TUTORIAL.8xv, ISLANDS.8xv, PLAINS.8xv)
.DEFAULT_GOAL := default

# ----------------------------

include $(shell cedev-config --makefile)

.PHONY: default world
default: $(BUILD) world

BIG_WORLDS = TUTORIAL ISLANDS PLAINS
world: $(BINDIR)/WORLD.8xv $(BIG_WORLDS:%=$(BINDIR)/%.8xv)
$(BINDIR)/WORLD.8xv: utils/make_world.py
	$(Q)mkdir -p $(BINDIR)
	$(Q)python3 utils/make_world.py $@
$(BINDIR)/%.8xv: utils/make_world.py
	$(Q)mkdir -p $(BINDIR)
	$(Q)python3 utils/make_world.py $@ --preset $(shell echo $* | tr A-Z a-z) --appvar $*
