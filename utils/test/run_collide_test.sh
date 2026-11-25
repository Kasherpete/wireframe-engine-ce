#!/usr/bin/env bash
# Builds the collision test (collide_test.c) against a copy of src/ and runs it in CEmu.
# Usage: utils/test/run_collide_test.sh [out_dir]. Screenshots in out_dir/shots, one every 2 s
# (about 16 emulated seconds: two scenarios of 8 s each, the second one shown).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$(realpath -m "${1:-/tmp/wireframe_collide}")"
rm -rf "$OUT"; mkdir -p "$OUT"
cp -r "$ROOT/src" "$ROOT/font" "$ROOT/makefile" "$OUT/"
cp "$ROOT/utils/test/collide_test.c" "$OUT/src/main.c"
# player.c reads the scripted keys
sed -i 's/^#include "player.h"$/#include "player.h"\nextern uint8_t test_keys[8];/; s/kb_Data\[\([0-9]\)\]/test_keys[\1]/g' "$OUT/src/player.c"
grep -q test_keys "$OUT/src/player.c"
python3 "$ROOT/utils/make_world.py" "$OUT/WORLD.8xv" --preset "${WORLD:-test}"
echo "CFLAGS += -DPROFILE=1" >> "$OUT/makefile"
(cd "$OUT" && make -j4 build 2>&1 | grep -iE "warning|error" || true)
PRG="$OUT/bin/$(sed -n "s/^NAME = //p" "$ROOT/makefile").8xp"
test -f "$PRG"
FILES="$OUT/WORLD.8xv" SHOTS=${SHOTS:-12} "$ROOT/utils/run_cemu.sh" "$PRG" "$OUT/shots" > /dev/null
ls "$OUT"/shots/t*.png
