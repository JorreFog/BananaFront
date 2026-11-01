#!/bin/bash
# devrun.sh <plus|rgds> <system> <core.so|core-name> <rom> <seconds> [tag]
# A core in BananaFront on a handheld, through the real menu launcher (banana-ui's lease, the game panel), for the
# Banana* emulator agents. Run it under banana-device-test's lock:
#   ~/.claude/skills/banana-device-test/scripts/locked.sh rgds -w 1800 -- devrun.sh rgds gba ./mgba_libretro.so stress.gba 120 base
# - Stages bananafront (BF_BIN, default this repo's build for the board), run-bananafront (BF_LAUNCHER) and the core
#   in /run/bdev/bf (tmpfs); the system's run line goes to run-bananafront by a bind-mounted systems.conf;
#   banana-ui with the lease for run-bananafront (BF_UI, default ~/ovn-front-kit/xb/ui/banana-ui) bind-mounted too.
# - The ROM is copied into /data/roms/<system>/ (the launch file only starts library games) unless a file of that
#   name is there; removed afterwards. Its save/state files are left (they're the run's data: copied back).
# - Collects: bananafront's log + its stats lines (fps, vblank, run ms avg/p50/p99, cpu %, audio underruns/fill/ratio)
#   every 10 s, the game-id file; quits through the game panel's pipe (QUIT); everything undone.
# PRE='<bash commands>': run on the PC after staging /run/bdev/bf, before the game starts (DEV and OUT set).
# MID='<bash commands>': run on the PC half-way (DEV and OUT set), e.g. to open the menu and take a picture.
# Results: ~/bf-runs/<rom>/<date>-<dev>-<tag>/  (bananafront.log, stats.txt, game-id, saves)
set -u
DEV=$1 SYS=$2 CORE=$3 ROM=$4 SEC=$5 TAG=${6:-run}
R=$(cd "$(dirname "$0")/.." && pwd)
BOARD=$([ "$DEV" = plus ] && echo rgds-plus || echo rgds)
BF_BIN=${BF_BIN:-$R/build/$BOARD/bananafront}
BF_LAUNCHER=${BF_LAUNCHER:-$HOME/ovn-front-bl/system/rootfs/usr/libexec/banana/run-bananafront}
BF_UI=${BF_UI:-$HOME/ovn-front-kit/xb/ui/banana-ui}
DS=~/.claude/skills/banana-device-test/scripts
dev() { ~/bananaos-0.4.3/dev.sh "$DEV" "$@"; }
B=$(basename "$ROM")
OUT=$HOME/bf-runs/${B%.*}/$(date +%Y%m%d-%H%M%S)-$DEV-$TAG; mkdir -p "$OUT"
K=$(mktemp -d); trap 'rm -rf "$K"' EXIT
dev "cat /etc/banana/systems.conf" > "$K/systems.conf.orig" || exit 1
case $CORE in */*) CN=$(basename "$CORE"); CARG=/run/bdev/bf/$CN ;; *) CN=; CARG=$CORE ;; esac
# the system's section: its run line -> the staged launcher wrapper
awk -v s="[$SYS]" -v r="run = /run/bdev/bf/run-bananafront $CARG %ROM%" '
  /^\[/ { in_s = ($0 == s) } in_s && /^run *=/ { print r; next } { print }' "$K/systems.conf.orig" > "$K/systems.conf"
grep -q "/run/bdev/bf/run-bananafront" "$K/systems.conf" || { echo "no [$SYS] run line"; exit 1; }
cat > "$K/run" <<W
#!/bin/sh
export BANANAFRONT_BIN=/run/bdev/bf/bananafront BANANAFRONT_STATS=/run/bdev/bf/stats.txt ${BF_ENV:-}
exec /run/bdev/bf/launcher "\$@"
W
dev 'rm -rf /run/bdev/bf; mkdir -p /run/bdev/bf'
for f in "$BF_BIN:bananafront" "$BF_LAUNCHER:launcher" "$K/run:run-bananafront" ${CN:+"$CORE:$CN"}; do
    dev "cat > /run/bdev/bf/${f##*:} && chmod +x /run/bdev/bf/${f##*:}" < "${f%%:*}" || exit 1
done
[ -n "${PRE:-}" ] && DEV=$DEV OUT=$OUT bash -c "$PRE"   # (after staging, before the start: e.g. a companion shm)
DROM=/data/roms/$SYS/$B
NEWROM=0
if dev "[ -e '$DROM' ]"; then echo "using the device's $DROM"; else
    dev "mkdir -p /data/roms/$SYS && cat > '$DROM'" < "$ROM" && NEWROM=1; fi
UIARG=; [ -f "$BF_UI" ] && UIARG="$BF_UI=/usr/bin/banana-ui"
$DS/deploy.sh "$DEV" --restart ui "$K/systems.conf=/etc/banana/systems.conf" $UIARG || exit 1
sleep 6
dev "echo '$DROM' > /run/banana/launch; for i in \$(seq 40); do pidof bananafront >/dev/null && break; sleep 0.5; done; pidof bananafront || echo 'bananafront did not start'"
if [ -n "${MID:-}" ]; then sleep $((SEC / 2)); DEV=$DEV OUT=$OUT bash -c "$MID"; sleep $((SEC - SEC / 2)); else sleep "$SEC"; fi
dev 'cat /run/banana/game-id' > "$OUT/game-id" 2>/dev/null
dev 'echo QUIT > /run/banana/retroarch.cmd; for i in $(seq 20); do pidof bananafront >/dev/null || break; sleep 0.5; done; pidof bananafront && kill $(pidof bananafront)'
sleep 1
dev 'cat /data/banana/logs/bananafront.log' > "$OUT/bananafront.log"
dev 'cat /run/bdev/bf/stats.txt' > "$OUT/stats.txt" 2>/dev/null
$DS/deploy.sh "$DEV" undo
[ $NEWROM = 1 ] && dev "rm -f '$DROM'"
dev 'rm -rf /run/bdev/bf'
grep -E "kms:|audio:|running|END" "$OUT/bananafront.log" | head -8
echo "-> $OUT"
