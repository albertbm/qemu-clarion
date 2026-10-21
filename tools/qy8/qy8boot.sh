#!/bin/bash
# Boot any NAND with any card image headless on the alex build and leave it running for QMP.
#
#   tools/qy8boot.sh <card.img> [-n nand.bin] [-m ze0|me0|ze1] [-b board] [-u] [-p] [-g] [-v] [-k]
#                               [-w workdir] [-s socket] [-t secs]
#
# No -m or -b: the board is read off the NAND (PROD block, QY model, NK versions) and
# printed. The bezel F-key table (jpn or row) is picked from the same model code and passed
# to the panel model, so F1-F9 mean the same function on every unit. The card's NK2 header
# and DB_INFO.TXT are compared with the NAND; a mismatch is a warning, the boot still
# runs. -u keeps the VEUP update request so the card's UP
# slot (LeafSDTools) boots instead of NK1. -p screendumps once the HMI is up and quits. -g opens a window.
# -t is the wait limit in seconds (240).
#
# Quiet by default: one summary line, the work dir, and the final state (ready, refused
# plus reason, or exited plus reason). qemu's own stdout and stderr, including the SGX
# render trace, go to <workdir>/qemu.log. -v prints everything instead: the full
# identification, the wait progress, and the tap/quit cheat sheet, and streams qemu's
# log live as well as writing it.
#
# Defaults to the DKL unit's patched flash and board qy8202na. The card is attached
# with QEMU's snapshot=on: writes land in a temporary overlay in TMPDIR (set to the
# work dir so it stays off the home volume), the card image itself is never opened
# for write, and no 16 GB clone appears anywhere. -k keeps the old behaviour instead:
# clone the card into the work dir with cp -c (free on APFS, real copy elsewhere) and
# boot that, for the rare run whose writes must survive the boot. The NAND copy gets
# its VEUP byte cleared, without which the unit boots the update kernel and the screen
# stays black forever. Use the alex build, not port: port's renderer announces its
# output is synthetic and every screendump comes back solid black.
set -e

A=/Users/albert/repos/qemu-clarion-wt/alex
B=/Users/albert/leaf-nand-backups
HERE=$(cd "$(dirname "$0")" && pwd)
T=$HERE

CARD=$1; shift || { sed -n '2,12p' "$0"; exit 1; }
NAND=$B/nand_qy8202na_14300241_00000338_PATCHED-130.bin
BOARD=; MODEL=; UPDATE=; SHOT=; VERBOSE=; SHOWKEYS=; KEEP=
W=~/work-qy8
SOCK=/tmp/qy8.qmp                # QEMU caps a unix path at 104 bytes, so keep it in /tmp
LIMIT=240

while getopts 'n:m:b:w:s:t:upgvk' o; do
  case $o in
    n) NAND=$OPTARG ;;
    m) MODEL=$OPTARG ;;
    b) BOARD=$OPTARG ;;
    w) W=$OPTARG ;;
    s) SOCK=$OPTARG ;;
    t) LIMIT=$OPTARG ;;
    u) UPDATE=1 ;;
    p) SHOT=1 ;;
    g) DISP=cocoa,show-cursor=on; SHOWKEYS=1 ;;
    v) VERBOSE=1 ;;
    k) KEEP=1 ;;
    *) sed -n '2,12p' "$0"; exit 1 ;;
  esac
done
[ -z "$VERBOSE" ] || [ -z "$SHOWKEYS" ] || echo "keyboard shortcuts: see tools/QY8KEYS.md"
[ -z "$VERBOSE" ] || [ -z "$KEEP" ] || echo "-k: cloning the card into the work dir, writes will persist there"

[ -f "$CARD" ] || { echo "no such card: $CARD" >&2; exit 1; }
[ -f "$NAND" ] || { echo "no such nand: $NAND" >&2; exit 1; }

case $MODEL in
  '') ;;
  ze0) BOARD=${BOARD:-qy8202na} ;;
  me0) BOARD=${BOARD:-qy8202pb} ;;
  ze1) BOARD=${BOARD:-qy8602nb} ;;
  *) echo "unknown model $MODEL (ze0, me0, ze1)" >&2; exit 1 ;;
esac

# Identify the NAND and the card, print it, and propose a board. Line 1 is the board.
IDENT=$(python3 - "$NAND" "$CARD" "$HERE/card" <<'EOF'
import re, sys
nand, card, fatdir = sys.argv[1:4]
sys.path.insert(0, fatdir)

def rd(path, off, n):
    with open(path, 'rb') as f:
        f.seek(off); return f.read(n)

def cstr(b):
    return b.split(b'\0')[0].split(b'\xff')[0].decode('latin1')

def nk2hdr(path, off):
    h = rd(path, off, 0x60)
    app = cstr(h[0x16:0x30])
    return (app if re.fullmatch(r'G2\d\d[A-Z]{4}\.\d{3}', app) else ''), cstr(h[0x50:0x60])

prod = rd(nand, 0x40000, 0x80)
if prod[:4] != b'PROD':
    print('qy8202na'); print('nand: no PROD block at 0x40000'); sys.exit()
code, qy, serial = prod[0x20:0x24].hex(), cstr(prod[0x40:0x48]), prod[0x50:0x54].hex()   # BCD
with open(nand, 'rb') as f:
    img = f.read()
vers = {}
for m in re.finditer(rb'(BL1\d|G[12]\d\d|SKINSK\d\d)[A-Z]{4}\.\d{3}', img):
    vers.setdefault(m.group(1)[:2].decode(), m.group(0).decode())
nav, part = nk2hdr(nand, 0xac0000)

known = {'QY8202NA': 'qy8202na', 'QY8202PB': 'qy8202pb', 'QY8602NB': 'qy8602nb',
         'QY8652NB': 'qy8652nb', 'QY8600NE': 'qy8600ne'}
if qy in known:
    board, why = known[qy], 'known model'
elif code.startswith('18'):
    board, why = 'qy8602nb', 'ZE1 family (model code 18...), closest known board'
elif qy.endswith('PB'):
    board, why = 'qy8202pb', 'ZE0 family, PB suffix'
else:
    board, why = 'qy8202na', 'ZE0 family (model code 14...), closest known board'
print(board)
print(f'unit {qy}  model {code}  NK1 {vers.get("G1", "?")} NK2 {vers.get("G2", "?")}'
      f' skin {vers.get("SK", "?")}  board {board}')
print(f'nand: model {code}  unit {qy}  serial {serial}  bootloader {vers.get("BL", "?")}')
print(f'      NK1 {vers.get("G1", "?")}  NK2 {vers.get("G2", "?")}  skin {vers.get("SK", "?")}'
      f'  nav slot {nav} ({part})')
print(f'      board {board}: {why}')

cnav, cpart = nk2hdr(card, 0x04d00000)
dbinfo = 'none'
try:
    from fatread import Fat
    fat = Fat(card); fat.open_part(fat.parts[0][2])
    d = {e[0]: e for e in fat.readdir()}.get('VER_INFO')
    if d:
        e = {e[0]: e for e in fat.readdir(d[2])}.get('DB_INFO.TXT')
        if e:
            dbinfo = ' '.join(fat.read_clusters(e[2], e[3]).decode('latin1').split())
except Exception as ex:
    dbinfo = f'unreadable ({ex})'
print(f'card: nav slot {cnav or "none"} ({cpart})  DB_INFO {dbinfo}')
if not cnav:
    print('      no NK2 in the card nav slot: an update card, boot it with -u')
elif cnav != nav:
    print(f'warning: card app {cnav} vs NAND app {nav}: expect Version Error / disc licence refusal')
EOF
)
[ -n "$BOARD" ] || BOARD=$(echo "$IDENT" | sed -n '1p')
SUMMARY=$(echo "$IDENT" | sed -n '2p')
echo "$SUMMARY"
[ -z "$VERBOSE" ] || echo "$IDENT" | tail -n +3

# Bezel panel table: third digit of the model code is 1 on a JPN unit (181...),
# anything else (EUR, USA, UK, ZE0, ME0) uses the row table. See REPORT-bezel-keys.md.
MCODE=$(echo "$IDENT" | sed -n 's/.*model \([0-9a-f]\{8\}\).*/\1/p' | head -1)
PANEL=row
[ "${MCODE:2:1}" = 1 ] && PANEL=jpn

mkdir -p "$W"; rm -f "$SOCK" "$W/console.log" "$W/boot.png"
echo "work $W"
cp "$NAND" "$W/nand.bin"
[ "$(xxd -s $((0x100000)) -l 4 -p "$W/nand.bin")" = 56455550 ] || [ -z "$VERBOSE" ] ||
  echo "warning: no VEUP block at 0x100000, update flag left alone" >&2
if [ -n "$UPDATE" ]; then
  printf '\x5a\0' | dd of="$W/nand.bin" bs=1 seek=$((0x100010)) conv=notrunc status=none
else
  printf '\0\0' | dd of="$W/nand.bin" bs=1 seek=$((0x100010)) conv=notrunc status=none
fi
if [ -n "$KEEP" ]; then
  cp -c "$CARD" "$W/card.img"; chmod u+w "$W/card.img"   # a base card is read-only
  SZ=$(stat -f %z "$W/card.img"); P2=1; while [ $P2 -lt $SZ ]; do P2=$((P2 * 2)); done
  [ $SZ -gt $((1 << 31)) ] || truncate -s $P2 "$W/card.img"   # QEMU wants SDSC sizes to be a power of two
  SDFILE="$W/card.img"; SNAP=
else
  SDFILE=$CARD; SNAP=,snapshot=on
fi

QARGS=(
  -M "clarion-qy8,board=$BOARD,dipsw=1,du-dotclk=33333333,gps=off"
  -global "clarion-dispmicom.panel=$PANEL"
  -drive "if=pflash,format=raw,file=$W/nand.bin"
  -drive "if=sd,index=0,format=raw,file=$SDFILE$SNAP"
  -display "${DISP:-none}"
  -serial "file:$W/console.log" -serial null -serial null -serial null
  -qmp "unix:$SOCK,server=on,wait=off"
)
# qemu's stdout+stderr (including the SGX render trace) always go to qemu.log; -v also
# streams them live. snapshot=on puts its overlay in TMPDIR, unlinked as soon as it
# opens it, so it costs no visible file and frees the space the moment qemu exits.
if [ -n "$VERBOSE" ]; then
  TMPDIR="$W" "$A/build/qemu-system-arm" "${QARGS[@]}" > >(tee "$W/qemu.log") 2>&1 &
else
  TMPDIR="$W" "$A/build/qemu-system-arm" "${QARGS[@]}" >"$W/qemu.log" 2>&1 &
fi
PID=$!
echo "$PID" > "$W/qemu.pid"

if [ -n "$VERBOSE" ]; then
  echo "pid $PID   board $BOARD   panel $PANEL   work $W   qmp $SOCK"
  echo "card $(basename "$CARD")   nand $(basename "$NAND")${UPDATE:+   update mode}"
fi

shot() {
  python3 "$T/qmp.py" "$SOCK" "screendump $W/boot.png -f png" >/dev/null 2>&1 || return 1
  [ -z "$VERBOSE" ] || echo "screen $W/boot.png"
}
quit() {
  python3 "$T/qmp.py" "$SOCK" quit >/dev/null 2>&1; sleep 2
  kill -0 $PID 2>/dev/null || [ -z "$VERBOSE" ] || echo "pid $PID gone"
}
fail() {
  [ -z "$VERBOSE" ] || echo
  echo "$1: $2"
  [ -z "$VERBOSE" ] || echo "see $W/console.log and $W/qemu.log" >&2
  shot; quit; exit 1
}

WHAT=HMI; [ -z "$UPDATE" ] || WHAT=updater
[ -z "$VERBOSE" ] || echo -n "waiting for the $WHAT "
STATE=
for ((i = 0; i < LIMIT; i += 5)); do
  kill -0 $PID 2>/dev/null || { [ -z "$VERBOSE" ] || cat "$W/qemu.log" >&2; fail exited "qemu exited"; }
  L=$W/console.log
  if grep -q 'Version Error\|DISCREFUSE' "$L" 2>/dev/null; then
    STATE=version; break
  elif grep -qi 'launch failed\|NorFlash Load failed' "$L" 2>/dev/null; then
    STATE=launch; break
  elif [ -n "$UPDATE" ] && grep -q 'Start log' "$L" 2>/dev/null; then
    STATE=ready; break
  elif [ -z "$UPDATE" ] && grep -q 'Start Verify Check' "$L" 2>/dev/null; then
    STATE=ready; break
  fi
  sleep 5; [ -z "$VERBOSE" ] || echo -n .
done
case $STATE in
  version) sleep 20; fail refused "version mismatch (card vs NAND app)" ;;
  launch) fail exited "launch failed" ;;
  '') fail exited "no HMI after ${LIMIT}s" ;;
esac
if [ -n "$VERBOSE" ] && grep -q 'Load failed' "$W/console.log"; then
  echo -n " (card NK2 refused, running the NAND copy)"
fi
for ((; i < LIMIT; i += 10)); do   # the marker lands 25 s or more before anything is drawn
  sleep 10; [ -z "$VERBOSE" ] || echo -n .
  shot >/dev/null && [ "$(stat -f %z "$W/boot.png")" -gt 3000 ] && break
done
[ "$(stat -f %z "$W/boot.png")" -gt 3000 ] || fail exited "screen still blank after ${LIMIT}s"
[ -z "$VERBOSE" ] || echo
echo ready
shot
[ -z "$SHOT" ] || { quit; exit 0; }
[ -z "$VERBOSE" ] || cat <<USAGE

  python3 $T/tap.py $SOCK X Y                      # 6 to 9 s between taps
  python3 $T/qmp.py $SOCK "screendump $W/s.png -f png"
  python3 $T/qmp.py $SOCK quit

  Navi 632 443   New Address 249 106   Country row 520 124
  Ten widget actions per screen, then leave the screen and come back.
USAGE
