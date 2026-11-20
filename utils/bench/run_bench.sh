#!/usr/bin/env bash
# Builds the benchmark / verification harness (bench.c) against a copy of src/ and runs it in CEmu.
#
# Usage: utils/bench/run_bench.sh [out_dir] [extra CFLAGS...]
#   utils/bench/run_bench.sh /tmp/bench -DBENCH_VIEWS=100                    timings only
#   utils/bench/run_bench.sh /tmp/bench -DFILL_REFERENCE -DBENCH_VIEWS=100   + compare with fill_ref.c
#   SCENE=far utils/bench/run_bench.sh ...                                   generated scene (scene.py)
#   SHOTS=14                                                                 screenshots (2 s apart)
# The results are on the last screenshot (out_dir/shots/tNN.png, printed at the end). The
# reference is slow: 100 views need about 14 screenshots, timings alone about 7.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$(realpath -m "${1:-/tmp/wireframe_bench}")"
shift || true
rm -rf "$OUT"; mkdir -p "$OUT"
cp -r "$ROOT/src" "$ROOT/font" "$ROOT/makefile" "$OUT/"
cp "$ROOT/utils/bench/bench.c" "$OUT/src/bench.inc"
sed -i 's/^int main(void)$/int real_main(void)/' "$OUT/src/main.c"
grep -q real_main "$OUT/src/main.c"
printf '\n#include "bench.inc"\n' >> "$OUT/src/main.c"
[ -n "$SCENE" ] && python3 "$ROOT/utils/bench/scene.py" "$SCENE" "$OUT/src/main.c"
echo "CFLAGS += -DSHOW_HUD=1 -Wno-shift-negative-value $*" >> "$OUT/makefile"
(cd "$OUT" && make -j4 2>&1 | grep -iE "warning|error" || true)
test -f "$OUT/bin/DEMOPRGM.8xp"
SHOTS=${SHOTS:-7} "$ROOT/utils/run_cemu.sh" "$OUT/bin/DEMOPRGM.8xp" "$OUT/shots" > /dev/null
ls "$OUT"/shots/t*.png | tail -1
