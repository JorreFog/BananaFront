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
exit $f
