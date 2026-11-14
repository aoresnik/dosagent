#!/bin/sh
# Builds the QEMU test images:
#   boot.img  FreeDOS 1.4 boot floppy, set up to skip the installer, load the
#             NE2000 packet driver and switch to C:
#   hdd.img   FAT16 hard disk with CWSDPMI, mTCP tools, configs, the CA
#             bundle and DOSAGENT.EXE (if built)
# Re-run after rebuilding DOSAGENT.EXE (only hdd.img is refreshed then).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DL=$HERE/dl
cd "$HERE"

if [ ! -f boot.img ]; then
    unzip -q -o "$DL/FD14-FloppyEdition.zip" '144m/x86BOOT.img' -d "$DL"
    cp "$DL/144m/x86BOOT.img" boot.img
    mdel -i boot.img ::setup.bat
    printf '!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\nSHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n' > fdconfig.tmp
    mcopy -o -i boot.img fdconfig.tmp ::FDCONFIG.SYS
    cat > fdauto.tmp <<'EOF'
@echo off
SET DOSDIR=A:\FREEDOS
SET PATH=A:\FREEDOS\BIN;C:\BIN
SET TEMP=C:\TEMP
SET WATTCP.CFG=C:\NET
SET MTCPCFG=C:\NET\MTCP.CFG
C:\NET\NE2000.COM 0x60 9 0x300
C:
CD \WORK
EOF
    sed 's/$/\r/' fdauto.tmp > fdauto.bat.tmp
    mcopy -o -i boot.img fdauto.bat.tmp ::FDAUTO.BAT
    rm -f fdconfig.tmp fdauto.tmp fdauto.bat.tmp
fi

rm -f hdd.img
# 64 MB FAT16 disk with an MBR partition table.
mformat -C -T 131072 -h 16 -s 32 -H 32 -i hdd.img@@16384 -v WORK ::
# Write a minimal MBR with one FAT16 partition starting at sector 32.
python3 -I - hdd.img <<'EOF'
import struct, sys
path = sys.argv[1]
with open(path, "r+b") as f:
    data = bytearray(f.read(512))
    start, count = 32, 131072 - 32
    # CHS values are placeholders; DOS uses the LBA fields for this layout.
    entry = struct.pack("<B3sB3sII", 0x80, b"\x00\x01\x00", 0x06, b"\xfe\xff\xff",
                        start, count)
    data[446:462] = entry
    data[510:512] = b"\x55\xaa"
    f.seek(0)
    f.write(data)
EOF
IMG="hdd.img@@16384"
mmd -i "$IMG" ::BIN ::NET ::TEMP ::WORK ::AGENT

unzip -q -o "$DL/pktd11.zip" NE2000.COM -d "$DL"
mcopy -o -i "$IMG" "$DL/NE2000.COM" ::NET/
unzip -q -o "$DL/csdpmi7b.zip" bin/CWSDPMI.EXE -d "$DL"
mcopy -o -i "$IMG" "$DL/bin/CWSDPMI.EXE" ::BIN/
mkdir -p "$DL/mtcp"
unzip -q -o "$DL/mtcp.zip" dhcp.exe ping.exe htget.exe dnstest.exe -d "$DL/mtcp"
for f in dhcp ping htget dnstest; do
    mcopy -o -i "$IMG" "$DL/mtcp/$f.exe" "::BIN/$(echo $f | tr a-z A-Z).EXE"
done

# Watt-32 uses DHCP by default; QEMU's user network provides it.
printf 'my_ip = dhcp\r\n' > wattcp.tmp
mcopy -o -i "$IMG" wattcp.tmp ::NET/WATTCP.CFG
printf 'PACKETINT 0x60\r\n' > mtcp.tmp
mcopy -o -i "$IMG" mtcp.tmp ::NET/MTCP.CFG
rm -f wattcp.tmp mtcp.tmp

# Agent files.
if [ -f "$ROOT/build/dos/DOSAGENT.EXE" ]; then
    mcopy -o -i "$IMG" "$ROOT/build/dos/DOSAGENT.EXE" ::AGENT/
fi
CA=${CACERT:-/etc/ssl/certs/ca-certificates.crt}
mcopy -o -i "$IMG" "$CA" ::AGENT/CACERT.PEM
# Every *.CFG here (DOSAGENT.CFG, LLAMA.CFG, ...) goes to C:\AGENT with CRLFs.
for cfg in "$HERE"/*.CFG; do
    [ -f "$cfg" ] || continue
    sed 's/$/\r/' "$cfg" > agentcfg.tmp
    mcopy -o -i "$IMG" agentcfg.tmp "::AGENT/$(basename "$cfg")"
    rm -f agentcfg.tmp
done
printf 'Hello from the DOS test disk.\r\n' > hello.tmp
mcopy -o -i "$IMG" hello.tmp ::WORK/HELLO.TXT
rm -f hello.tmp
mdir -/ -i "$IMG" ::
