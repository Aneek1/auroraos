#!/bin/bash
# AuroraOS 09 — GRUB (UEFI). Run INSIDE the chroot; /boot/efi must be mounted.
set -e
. /aurora/config/build.conf
mountpoint -q /boot/efi || { echo "!! /boot/efi not mounted"; exit 1; }
mountpoint -q /sys/firmware/efi/efivars || \
  mount -v -t efivarfs efivarfs /sys/firmware/efi/efivars 2>/dev/null || \
  echo "(efivars unavailable — NVRAM entry will be skipped; using removable path)"

grub-install --target=$GRUB_TARGET --efi-directory=/boot/efi \
  --bootloader-id=AuroraOS --removable || \
grub-install --target=$GRUB_TARGET --efi-directory=/boot/efi --bootloader-id=AuroraOS

ROOTDEV=$(grep ROOT /.aurora-disk 2>/dev/null | cut -d= -f2)
cat > /boot/grub/grub.cfg <<EOF
set default=0
set timeout=2

insmod part_gpt
insmod ext2
insmod all_video
set gfxpayload=keep

menuentry "AuroraOS 1.0 — daybreak" {
  linux /boot/vmlinuz-aurora root=${ROOTDEV:-/dev/sda2} rw quiet loglevel=3 vt.global_cursor_default=0
}

menuentry "AuroraOS (verbose / rescue)" {
  linux /boot/vmlinuz-aurora root=${ROOTDEV:-/dev/sda2} rw systemd.unit=multi-user.target
}
EOF
echo "== 09 complete — run /aurora/scripts/10-aurora-shell.sh =="
