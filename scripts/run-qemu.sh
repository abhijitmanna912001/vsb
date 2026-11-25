#!/bin/bash
# Boots the QNX 8.0 aarch64 image built by mkqnximage in QEMU (software
# emulation). Usage: scripts/run-qemu.sh [path-to-mkqnximage-dir]
# Devices are virtio-mmio, added in this order so they land in the slots the
# QNX image expects: disk, network, random generator.
DIR=${1:-$HOME/qnxprojects/qnx-vm}
cd "$DIR"
qemu-system-aarch64 \
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
