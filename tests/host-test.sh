#!/bin/bash
# host-test.sh <core.so> <rom.gb>: BananaFront's PC end-to-end checks with the null drivers (correctness only):
# the save written and read back, a state saved/loaded in RetroArch's format with its thumbnail, the command pipe
# (RetroArch's words), READ_CORE_MEMORY answered, the automatic state for quick resume, a screenshot.
set -u
BF=${BF:-$(dirname "$0")/../build/host/bananafront}
CORE=$1 ROM=$2
T=$(mktemp -d "${TMPDIR:-/tmp}/bf-host.XXXXXX")
trap 'rm -rf "$T"' EXIT
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
export ASAN_OPTIONS=detect_leaks=0 BANANAFRONT_INPUT=/dev/null
run() {  # run <stdin-script> <extra args...>: the script's lines fed to the command pipe 0.3 s apart
    local script=$1; shift
    mkfifo "$T/cmd"
    BANANAFRONT_NULL_HZ=60 "$BF" --saves "$T/saves" --states "$T/states" --shots "$T/shots" --bios "$T/bios" \
        --config "$T/cfg" "$@" "$CORE" "$ROM" < "$T/cmd" > "$T/out" 2> "$T/err" &
    local pid=$!
    exec 7> "$T/cmd"
    while IFS= read -r l; do sleep 0.3; echo "$l" >&7; done <<< "$script"
    sleep 0.3
    exec 7>&-
    wait $pid
    echo "exit $?" >> "$T/err"
    rm -f "$T/cmd"
}
G=$(basename "${ROM%.*}")
run $'READ_CORE_MEMORY c000 4\nSAVE_STATE\nSTATE_SLOT_PLUS\nSAVE_STATE\nSCREENSHOT\nLOAD_STATE_SLOT 0\nQUIT'
LIB=$(sed -n 's/^bananafront: core \([^ ]*\) .*/\1/p' "$T/err")
check "clean exit" "grep -q '^exit 0' $T/err"
check "save written" "[ -s '$T/saves/$LIB/$G.srm' ]"
check "state slot 0 (RetroArch name, RZIP)" "head -c 8 '$T/states/$LIB/$G.state' | grep -q '#RZIPv'"
check "state slot 1" "[ -s '$T/states/$LIB/$G.state1' ]"
check "thumbnail" "[ -s '$T/states/$LIB/$G.state.png' ]"
check "screenshot" "ls $T/shots/$G-*.png >/dev/null 2>&1"
check "state loaded" "grep -q 'state loaded' $T/err"
check "memory reply" "grep -Eq '^READ_CORE_MEMORY c000( [0-9A-F]{2}){4}$|^READ_CORE_MEMORY c000 -1' $T/out"
grep '^READ_CORE_MEMORY' "$T/out" | head -2
cp "$T/saves/$LIB/$G.srm" "$T/srm1"
# second run: the save read back (the game counts on from it), quick resume written and then loaded
run $'QUIT' --resume-save
check "save loaded" "grep -q 'save: ' $T/err"
check "auto state written" "[ -s '$T/states/$LIB/$G.state.auto' ]"
run $'QUIT' --resume-load
check "auto state loaded" "grep -q 'state loaded: .*state.auto' $T/err"
# SIGUSR1 (bananad, Power off after sleeping): the automatic state whatever the setting, then quit
rm -f "$T/states/$LIB/$G.state.auto"
mkfifo "$T/cmd"
BANANAFRONT_NULL_HZ=60 "$BF" --saves "$T/saves" --states "$T/states" --shots "$T/shots" --bios "$T/bios" \
    --config "$T/cfg" "$CORE" "$ROM" < "$T/cmd" > "$T/out" 2> "$T/err" &
pid=$!
exec 7> "$T/cmd"
sleep 1; kill -USR1 $pid; wait $pid; s=$?
exec 7>&-
check "SIGUSR1 quits (status $s)" "[ $s = 0 ]"
check "SIGUSR1 writes the auto state" "[ -s '$T/states/$LIB/$G.state.auto' ]"
grep -E 'END|ERROR|runtime error' "$T/err" | head -3
[ $fails = 0 ] && echo "all host checks passed" || { echo "$fails failed"; exit 1; }
