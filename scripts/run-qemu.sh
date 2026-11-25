#!/bin/bash
# Boots a QNX 8.0 aarch64 image in QEMU on an Apple Silicon Mac.
#
# Usage: scripts/run-qemu.sh [image-dir]
#   image-dir is the output of `mkqnximage --type=qemu --arch=aarch64le`
#   (default: ~/qnxprojects/qnx-vm). It must contain output/ifs.bin and
#   output/disk-qemu.
#
# Notes:
#  - TCG (software emulation) is used. HVF with -cpu host fails on a GICv3
#    assertion with this QNX image.
#  - virtio-mmio devices (the "-device virtio-*-device" forms) are required.
#    QNX's virtio drivers do not find PCI virtio devices on this machine.
#  - The devices must stay in this order: block, net, rng.
#  - Guest SSH (port 22) is forwarded to localhost:2222.
#  - To quit QEMU: press Ctrl+A, then X.
set -e
DIR=${1:-$HOME/qnxprojects/qnx-vm}
cd "$DIR"
exec qemu-system-aarch64 \
  -machine virt,gic-version=3 \
  -accel tcg -cpu cortex-a57 \
  -smp 2 -m 1G \
  -nographic -serial mon:stdio \
  -kernel output/ifs.bin \
  -drive file=output/disk-qemu,if=none,id=drv0,format=raw \
  -device virtio-blk-device,drive=drv0 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-device,netdev=net0 \
  -object rng-random,filename=/dev/urandom,id=rng0 \
  -device virtio-rng-device,rng=rng0
