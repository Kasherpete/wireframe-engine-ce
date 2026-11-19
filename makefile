# ----------------------------
# Makefile Options
# ----------------------------

NAME = DEMOPRGM
#ICON = icon.png
DESCRIPTION = "A Fast Wireframe Engine for the TI84+ CE"
COMPRESSED = YES

COMPRESSED_MODE = zx0
ARCHIVED = NO

CFLAGS = -Wall -Wextra -O3 -ffast-math
CXXFLAGS = -Wall -Wextra -O3 -ffast-math

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

# ----------------------------

include $(shell cedev-config --makefile)

.PHONY: hud
hud: $(BUILD)
