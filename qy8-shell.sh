#!/bin/bash
# Boot a head unit in NORM(EVA) and attach the debug shell to this terminal.
# Usage: ./qy8-shell.sh [NAND] [CARD] [DIPSW]
# BOARD=ze0 boots the 2014-2017 unit: same NAND and card arguments.
set -e
here=$(cd "$(dirname "$0")" && pwd)
board=${BOARD:-ze1}
if [ "$board" = ze0 ]; then
    nand=${1:-$QY8_NAND}
    card=${2:-$QY8_CARD}
    glsyms=$here/contrib/plugins/qy8gl-g214.syms
else
    nand=${1:-$QY8_NAND}
    card=${2:-$QY8_CARD}
    glsyms=$here/contrib/plugins/qy8gl.syms
fi
mode=${3:-1}
work=$(mktemp -d /tmp/qy8.XXXXXX)

# the guest writes to both, so run on copies: flash by copy, card by APFS clone
cp "$nand" "$work/nand.bin"
# leafsdtools dumps carry VEUP 'Z' (update requested), which boots the update kernel instead of NK1
if [ -z "$KEEP_VEUP" ]; then
    printf '\x00\x00' | dd of="$work/nand.bin" bs=1 seek=$((0x100010)) conv=notrunc 2>/dev/null
fi
cp -c "$card" "$work/card.img"
truncate -s 16G "$work/card.img"

display=cocoa
[ -n "$HEADLESS" ] && display=none

# software-render the AUI's GL calls; NOGL=1 leaves the screen to the SGX model.
# The immobiliser check reads as disabled (config 0x0f = 0, as leafsdtools
# sets it); IMMO=1 keeps the stored value.
gl=()
if [ -z "$NOGL" ]; then
    export QY8_GL_FRAME="$work/glframe.bin"
    gl=(-plugin "$here/build/contrib/plugins/libqy8gl.dylib,syms=$glsyms,log=$work/gl.log")
    [ -z "${IMMO:-}" ] && gl[1]="${gl[1]},cnf=0x0f:0"
fi

echo "work dir: $work   (Ctrl-A X quits)"
exec "$here/build/qemu-system-arm" -M clarion-qy8,board="$board",dipsw="$mode",du-dotclk=33333333 \
    -drive if=pflash,format=raw,file="$work/nand.bin" \
    -drive if=sd,index=0,format=raw,file="$work/card.img" \
    "${gl[@]}" -display "$display" -serial mon:stdio 2>"$work/qemu.log"
