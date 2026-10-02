#!/bin/bash
# Boot the ZE1 bench unit in NORM(EVA) and attach the debug shell to this terminal.
# Usage: ./qy8-shell.sh [NAND] [CARD] [DIPSW]
set -e
here=$(cd "$(dirname "$0")" && pwd)
nand=${1:-$QY8_NAND}
card=${2:-$QY8_CARD}
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

# software-render the AUI's GL calls; NOGL=1 leaves the screen to the SGX model
gl=()
if [ -z "$NOGL" ]; then
    export QY8_GL_FRAME="$work/glframe.bin"
    gl=(-plugin "$here/build/contrib/plugins/libqy8gl.dylib,syms=$here/contrib/plugins/qy8gl.syms,log=$work/gl.log")
fi

echo "work dir: $work   (Ctrl-A X quits)"
exec "$here/build/qemu-system-arm" -M clarion-qy8,dipsw="$mode",du-dotclk=33333333 \
    -drive if=pflash,format=raw,file="$work/nand.bin" \
    -drive if=sd,index=0,format=raw,file="$work/card.img" \
    "${gl[@]}" -display "$display" -serial mon:stdio 2>"$work/qemu.log"
