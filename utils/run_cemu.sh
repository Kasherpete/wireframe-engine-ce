#!/usr/bin/env bash
# Run a build in CEmu and take screenshots, without touching your CEmu config.
#
# Usage: utils/run_cemu.sh [program.8xp] [out_dir]
#   defaults: bin/DEMOPRGM.8xp, /tmp/cemu_run (bin/DEMOFOG.8xp: the fog build)
#   KEYS='"key|right","delay|500"'  extra autotester sequence items after launch
#   SHOTS=12                        number of screenshots (one every 2 s)
#   FILES="a.8xv b.8xv"             other files to send (default: bin/WORLD.8xv, the world)
#
# How it works: boots a clean calculator (TI-OS 5.8 blocks native ASM, so it also sends
# arTIfiCE as prgmA), runs prgmA via key presses to open the arTIfiCE shell, launches
# the program from there, then grabs screenshots over CEmu's IPC (--id / --screenshot).
# Caveats: the emulator runs the key sequence faster than wall-clock time, so KEYS
# timing relative to the screenshots is loose. dbg_printf output is not captured.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PRG="$(realpath "${1:-$ROOT/bin/DEMOPRGM.8xp}")"
OUT="$(realpath -m "${2:-/tmp/cemu_run}")"
ROM="${CEMU_ROM:-$HOME/Downloads/ROM_IMAGE_2.rom}"
CLIBS="${CEMU_CLIBS:-$HOME/Downloads/clibs.8xg}"
ARTIFICE="${CEMU_ARTIFICE:-$HOME/Downloads/arTIfiCE_v2.1.8xp}"
SHOTS="${SHOTS:-12}"
ID=wireframe-test

mkdir -p "$OUT"; rm -f "$OUT"/t*.png
export QT_QPA_PLATFORM=xcb   # the static Qt build has no wayland plugin

stop_cemu() { for p in $(pgrep -x cemu); do kill "$p"; done; }
stop_cemu; sleep 1

NAME="$(basename "$PRG" .8xp)"   # the on-calc name (DEMOPRGM, or DEMOFOG for make fog)
cp "$CLIBS" "$OUT/clibs.8xg"; cp "$ARTIFICE" "$OUT/A.8xp"; cp "$PRG" "$OUT/$NAME.8xp"
FILES="${FILES-$( [ -f "$ROOT/bin/WORLD.8xv" ] && echo "$ROOT/bin/WORLD.8xv" || true )}"
EXTRA=""
for f in $FILES; do cp "$f" "$OUT/"; EXTRA="$EXTRA, \"$(basename "$f")\""; done
cat > "$OUT/run.json" <<EOF
{
  "transfer_files": ["clibs.8xg", "A.8xp"$EXTRA, "$NAME.8xp"],
  "target": {"name": "$NAME", "isASM": true},
  "sequence": [
    "delay|2000", "key|prgm", "delay|500", "key|enter", "delay|300", "key|enter",
    "delay|3000", "key|enter",
    "delay|3000"${KEYS:+, $KEYS}, "delay|600000"
  ],
  "hashes": {}
}
EOF

(cd "$OUT" && cemu --no-settings -n --no-test-dialog -r "$ROM" --id "$ID" -t "$OUT/run.json" >"$OUT/log" 2>&1 &)
sleep 8
for i in $(seq -w 1 "$SHOTS"); do
    timeout 10 cemu --id "$ID" --screenshot "$OUT/t$i.png" >/dev/null 2>&1 || true
    sleep 2
done
stop_cemu
ls "$OUT"/t*.png
