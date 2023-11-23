# ----------------------------
# Makefile Options
# ----------------------------

NAME = DEMOPRGM
#ICON = icon.png
DESCRIPTION = "A Fast Wireframe Engine for the TI84+ CE"
COMPRESSED = YES

COMPRESSED_MODE = zx0
ARCHIVED = NO

CFLAGS = -Wall -Wextra -Oz #-Wconversion -ffast-math
CXXFLAGS = -Wall -Wextra -Oz #-Wconversion -ffast-math

# ----------------------------

include $(shell cedev-config --makefile)