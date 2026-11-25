#!/bin/bash
# run-qemu.sh — boot the VitalGuard QNX 8.0 target image under QEMU.
#
# The image is built with mkqnximage from the SDP 8.0 baseline (see
# qemu/README.md for the recipe and the VitalGuard snippets); the QSTI
# package is not required.  The pipeline autostarts at boot — after
# "Startup complete" you land on a root shell with vitalgen / vitaldsp /
# alarmgr already running under vgsupervisor.  Drive it with faultctl
# (vfib / desat / apnea / kill dsp / ack / status).
#
# Interactive use: the serial console is on stdio (-nographic);
# exit QEMU with Ctrl-A x.
#
# VG_IMAGE_DIR can point at another mkqnximage output directory.

set -eu

IMG_DIR="${VG_IMAGE_DIR:-$HOME/workspace/qnx/custom-image/build/output}"

if [ ! -f "$IMG_DIR/ifs.bin" ] || [ ! -f "$IMG_DIR/disk-qemu" ]; then
    echo "QNX image not found in: $IMG_DIR" >&2
    echo "Build it first (mkqnximage) — see qemu/README.md." >&2
    exit 1
fi

exec qemu-system-x86_64 \
    -machine pc -smp 2 -cpu max -m 2G \
    -kernel "$IMG_DIR/ifs.bin" \
    -drive file="$IMG_DIR/disk-qemu",format=raw,if=ide,id=drv0 \
    -object rng-random,filename=/dev/urandom,id=rng0 \
    -device virtio-rng-pci,rng=rng0 \
    -netdev user,id=net0 \
    -device virtio-net-pci,netdev=net0 \
    -nographic
