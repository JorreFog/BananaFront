#!/bin/bash
# core-test.sh: BananaFront answers the environment as RetroArch does (the experimental-bit commands included), and
# READ_CORE_MEMORY resolves the core's map; with tests/testcore.c (make test builds it)
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d "${TMPDIR:-/tmp}/bf-core.XXXXXX"); trap 'rm -rf "$T"' EXIT
echo x > "$T/game.tst"; mkfifo "$T/cmd"
ASAN_OPTIONS=detect_leaks=0 BANANAFRONT_INPUT=/dev/null ./build/host/bananafront --saves "$T/s" --states "$T/st" \
    --shots "$T/sh" --config "$T/c" build/host/testcore.so "$T/game.tst" < "$T/cmd" > "$T/out" 2> "$T/err" &
exec 7> "$T/cmd"; sleep 0.5; echo "READ_CORE_MEMORY c100 8" >&7; sleep 0.3; echo QUIT >&7; sleep 0.5; exec 7>&-; wait
f=0
for q in pixel_format input_bitmasks support_achievements fastforwarding memory_maps; do
    grep -q "testcore: $q = 1" "$T/err" && echo "ok   $q answered" || { echo "FAIL $q not answered"; f=1; }
done
grep -q "^READ_CORE_MEMORY c100 54 45 53 54 43 4F 52 45$" "$T/out" && echo "ok   READ_CORE_MEMORY through the map" || { echo "FAIL memory: $(cat $T/out)"; f=1; }
# the map sent from the first retro_run (mGBA does that): READ_CORE_MEMORY and the companion export see it
mkfifo "$T/cmd2"
TESTCORE_LATE_MAPS=1 ASAN_OPTIONS=detect_leaks=0 BANANAFRONT_INPUT=/dev/null ./build/host/bananafront --saves "$T/s" \
    --states "$T/st" --shots "$T/sh" --config "$T/c" build/host/testcore.so "$T/game.tst" < "$T/cmd2" > "$T/out2" 2> "$T/err2" &
exec 7> "$T/cmd2"; sleep 0.5; echo "READ_CORE_MEMORY c100 8" >&7; sleep 0.3; echo QUIT >&7; sleep 0.5; exec 7>&-; wait
grep -q "^READ_CORE_MEMORY c100 54 45 53 54 43 4F 52 45$" "$T/out2" && echo "ok   a map sent after the load is used" || { echo "FAIL late map: $(cat $T/out2)"; f=1; }
# a heavy core (9 ms frames: two never fit in a refresh) with a spike (22 ms) every 60 frames: each owed frame is caught up while the flip waits
TESTCORE_FRAME_MS=9 TESTCORE_SPIKE_EVERY=60 ASAN_OPTIONS=detect_leaks=0 BANANAFRONT_INPUT=/dev/null BANANAFRONT_FLOOR=0 \
    ./build/host/bananafront --saves "$T/s" --states "$T/st" --shots "$T/sh" --config "$T/c" --frames 420 \
    build/host/testcore.so "$T/game.tst" < /dev/null > /dev/null 2> "$T/err3"
c=$(grep -o "END .*caught-up [0-9]*" "$T/err3" | grep -o "caught-up [0-9]*" | grep -o "[0-9]*$")
[ "${c:-0}" -ge 4 ] && echo "ok   a heavy core's owed frames caught up ($c)" || { echo "FAIL catch-up: $(grep END "$T/err3")"; f=1; }
exit $f
