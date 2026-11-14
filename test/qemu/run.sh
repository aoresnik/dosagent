#!/bin/sh
# Boots the FreeDOS test machine: 486 (QEMU has no 386 model), 4 MB RAM,
# NE2000 ISA card on QEMU's user-mode network (the host is 10.0.2.2).
# Pass -headless to run without a window; then drive it with qemuctl.py.
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"
DISPLAY_OPT=""
if [ "$1" = "-headless" ]; then
    DISPLAY_OPT="-display none"
    shift
fi
exec qemu-system-i386 -cpu 486 -m 4 \
    -drive file=boot.img,format=raw,if=floppy \
    -drive file=hdd.img,format=raw,if=ide \
    -boot a \
    -netdev user,id=n0 -device ne2k_isa,netdev=n0,iobase=0x300,irq=9 \
    -monitor unix:"$HERE/monitor.sock",server,nowait \
    $DISPLAY_OPT "$@"
