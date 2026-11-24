#!/bin/bash
# run-qemu.sh — boot the VitalGuard QNX target image under QEMU.
#
# *** WORK IN PROGRESS — NOT YET VERIFIED ***
# This script is a scaffold written before the QNX SDP 8.0 was
# available on this project.  It encodes the intended invocation
# (Quick Start Target Image for QEMU from the SDP) so the target
# bring-up is a fill-in-the-paths exercise, but it has NOT been run
# against a real SDP installation.  Expect to adjust:
#   - the QSTI image path/name for the installed SDP version,
#   - how the VitalGuard binaries + slm config get into the image
#     (mkqnximage CTI build is the planned route),
#   - QEMU device options for the SDP's QSTI.
#
# Prerequisites: QNX SDP 8.0 installed and its environment sourced
# (qnxsdp-env.sh), QEMU on PATH.

set -eu

QNX_TARGET="${QNX_TARGET:?source the SDP environment (qnxsdp-env.sh) first}"

# Quick Start Target Image shipped with the SDP (name per SDP 8.0;
# verify against the installed tree).
QSTI_IMG="${VG_QSTI_IMG:-$QNX_TARGET/x86_64/boot/qsti-qemu.qcow2}"

if [ ! -f "$QSTI_IMG" ]; then
    echo "QSTI image not found: $QSTI_IMG" >&2
    echo "Set VG_QSTI_IMG to the Quick Start Target Image from your SDP." >&2
    exit 1
fi

exec qemu-system-x86_64 \
    -m 512M \
    -cpu max \
    -drive file="$QSTI_IMG",format=qcow2,if=ide \
    -netdev user,id=net0 -device e1000,netdev=net0 \
    -nographic \
    -serial mon:stdio
