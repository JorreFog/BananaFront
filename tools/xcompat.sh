#!/bin/bash
# xcompat.sh <plus|rgds> <system> <core-name> <rom> [secs]
# Saves and states carried over between RetroArch and BananaFront, both ways, on a handheld through the menu:
#   1 RetroArch: plays, saves a state (slot 0), quits (its .srm written)
#   2 BananaFront: reads that .srm, loads RetroArch's state, saves slot 1, quits (its .srm written)
#   3 RetroArch: loads BananaFront's slot 1 state, quits
# Pass: each step's log says it read/loaded the other's file. The ROM's own save/state files are removed first and
# after (use a test ROM, never an owner's game). Run under the lock.
set -u
DEV=$1 SYS=$2 CORE=$3 ROM=$4 SEC=${5:-12}
R=$(cd "$(dirname "$0")/.." && pwd)
BOARD=$([ "$DEV" = plus ] && echo rgds-plus || echo rgds)
DS=~/.claude/skills/banana-device-test/scripts
dev() { ~/bananaos-0.4.3/dev.sh "$DEV" "$@"; }
B=$(basename "$ROM"); G=${B%.*}
OUT=$HOME/bf-runs/xcompat-$G/$(date +%Y%m%d-%H%M%S)-$DEV; mkdir -p "$OUT"
K=$(mktemp -d); trap 'rm -rf "$K"' EXIT
dev "cat /etc/banana/systems.conf" > "$K/systems.orig" || exit 1
mk_conf() { awk -v s="[$SYS]" -v r="run = $1" '/^\[/ { in_s = ($0 == s) } in_s && /^run *=/ { print r; next } { print }' "$K/systems.orig" > "$2"; }
mk_conf "/usr/libexec/banana/run-retroarch $CORE %ROM%" "$K/sys-ra.conf"
mk_conf "/run/bdev/bf/run-bananafront $CORE %ROM%" "$K/sys-bf.conf"
printf '#!/bin/sh\nexport BANANAFRONT_BIN=/run/bdev/bf/bananafront\nexec /run/bdev/bf/launcher "$@"\n' > "$K/wrap"
dev 'rm -rf /run/bdev/bf; mkdir -p /run/bdev/bf'
for f in "$R/build/$BOARD/bananafront:bananafront" "$HOME/ovn-front-bl/system/rootfs/usr/libexec/banana/run-bananafront:launcher" \
         "$K/wrap:run-bananafront" "$K/sys-ra.conf:sys-ra.conf" "$K/sys-bf.conf:sys-bf.conf"; do
    dev "cat > /run/bdev/bf/${f##*:} && chmod +x /run/bdev/bf/${f##*:}" < "${f%%:*}" || exit 1
done
DROM=/data/roms/$SYS/$B
dev "[ -e '$DROM' ]" && { echo "$DROM is there already: not a test ROM's place"; exit 1; }
dev "mkdir -p /data/roms/$SYS && cat > '$DROM'" < "$ROM"
clean() { dev "rm -f /data/saves/*/'$G'.srm /data/saves/*/'$G'.rtc /data/states/*/'$G'.state* 2>/dev/null"; }
clean
[ -f "$HOME/ovn-front-kit/xb/ui/banana-ui" ] && $DS/deploy.sh "$DEV" --restart ui "$HOME/ovn-front-kit/xb/ui/banana-ui=/usr/bin/banana-ui" > /dev/null
run() {  # run <ra|bf> <commands at SEC-3 s, ';'-separated> <tag>
    local fe=$1 cmds=$2 tag=$3 name
    [ $fe = ra ] && name=retroarch || name=bananafront
    dev "umount -l /etc/banana/systems.conf 2>/dev/null; mount --bind /run/bdev/bf/sys-$fe.conf /etc/banana/systems.conf; killall banana-ui"
    sleep 6
    dev "rm -f /run/banana/retroarch.log /data/banana/logs/bananafront.log; echo '$DROM' > /run/banana/launch; for i in \$(seq 40); do pidof $name >/dev/null && break; sleep 0.5; done"
    sleep $((SEC - 4))
    local IFS=';'
    for c in $cmds; do dev "echo '$c' > /run/banana/retroarch.cmd"; sleep 1.5; done
    unset IFS
    dev "echo QUIT > /run/banana/retroarch.cmd; for i in \$(seq 20); do pidof $name >/dev/null || break; sleep 0.5; done"
    sleep 1
    dev "cat /run/banana/retroarch.log 2>/dev/null; cat /data/banana/logs/bananafront.log 2>/dev/null" > "$OUT/$tag.log"
    dev "ls -la --full-time /data/saves/*/'$G'.* /data/states/*/'$G'.* 2>/dev/null; md5sum /data/saves/*/'$G'.srm 2>/dev/null" > "$OUT/$tag.files"
}
run ra "SAVE_STATE" 1-ra
run bf "LOAD_STATE;SAVE_STATE_SLOT 1" 2-bf
run ra "STATE_SLOT_PLUS;LOAD_STATE;STATE_SLOT_MINUS" 3-ra   # (the slot back to 0: RetroArch keeps it in its config)
dev 'umount -l /etc/banana/systems.conf 2>/dev/null'
$DS/deploy.sh "$DEV" undo > /dev/null
clean
dev "rm -f '$DROM'; rm -rf /run/bdev/bf"
echo "== 1 RetroArch: state + save written"; grep -E "state|Saving|SRAM|srm" "$OUT/1-ra.log" | grep -i -E "sav|state" | head -4; cat "$OUT/1-ra.files"
echo "== 2 BananaFront: RetroArch's save + state read, slot 1 written"; grep -E "save:|state (loaded|saved|NOT)" "$OUT/2-bf.log"; cat "$OUT/2-bf.files"
echo "== 3 RetroArch: BananaFront's slot 1 loaded"; grep -i -E "load.*state|state.*load" "$OUT/3-ra.log" | head -4
echo "-> $OUT"
