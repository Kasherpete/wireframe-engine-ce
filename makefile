# ----------------------------
# Makefile Options
# ----------------------------

NAME = DEMO
#ICON = icon.png
DESCRIPTION = "CE C Toolchain Demo"
COMPRESSED = YES

COMPRESSED_MODE = zx0
ARCHIVED = NO

CFLAGS = -Wall -Wextra -O3
CXXFLAGS = -Wall -Wextra -O3

# ----------------------------

include $(shell cedev-config --makefile)
