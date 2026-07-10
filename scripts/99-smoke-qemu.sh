#!/bin/bash
# AuroraOS 99 — boot the live ISO in QEMU (smoke test). Run on the build host.
set -e
. "$(dirname "$0")/../config/build.conf"
ISO=${1:?usage: 99-smoke-qemu.sh <iso>}
case "$AURORA_ARCH" in
  aarch64)
    FW=$(ls /usr/share/AAVMF/AAVMF_CODE.fd \
            /usr/share/qemu-efi-aarch64/QEMU_EFI.fd 2>/dev/null | head -1)
    [ -n "$FW" ] || { echo "!! install qemu-efi-aarch64 (edk2 firmware)"; exit 1; }
    exec qemu-system-aarch64 -M virt -cpu max -smp 4 -m 4G -bios "$FW" \
      -device virtio-gpu-pci -device qemu-xhci -device usb-kbd -device usb-tablet \
      -device virtio-scsi-pci -device scsi-cd,drive=cd0 \
      -drive if=none,id=cd0,media=cdrom,file="$ISO"
    ;;
  x86_64)
    exec qemu-system-x86_64 -enable-kvm -m 4G -smp 4 -vga virtio \
      -bios /usr/share/ovmf/OVMF.fd -cdrom "$ISO"
    ;;
esac
