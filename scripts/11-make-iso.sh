#!/bin/bash
# AuroraOS 11 — optional live ISO. Run on the HOST (not chroot) after 10.
# Needs on host: squashfs-tools, xorriso, cpio, mtools, and grub EFI tools for
# the build arch (x86_64: grub-efi-amd64-bin + grub-pc-bin; aarch64: grub-efi-arm64-bin).
set -e
. "$(dirname "$0")/../config/build.conf"
OUT=${1:-auroraos-${DISTRO_VERSION}.iso}
WORK=$(mktemp -d)

echo "== 1/4 squashing root filesystem (this takes a while) =="
mkdir -p "$WORK/iso/live" "$WORK/iso/boot/grub"
mksquashfs "$LFS" "$WORK/iso/live/rootfs.squashfs" \
  -comp zstd -e boot/efi -e sources -e proc -e sys -e dev -e run -e tmp -e aurora

echo "== 2/4 building live initramfs =="
IR="$WORK/initramfs"; mkdir -p "$IR"/{bin,dev,proc,sys,mnt/{cd,rw,ro,newroot}}
cp -a "$LFS"/usr/bin/busybox "$IR/bin/" 2>/dev/null || {
  # no busybox in LFS base — build static busybox quickly on host if available
  command -v busybox >/dev/null && cp "$(command -v busybox)" "$IR/bin/busybox" || {
    echo "!! need a static busybox at \$LFS/usr/bin/busybox or on the host"; exit 1; }
}
for a in sh mount switch_root sleep mkdir findfs blkid modprobe; do ln -sf busybox "$IR/bin/$a"; done
cat > "$IR/init" <<"EOF"
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
# wait for the CD device
i=0; while [ $i -lt 20 ]; do
  for d in /dev/sr0 /dev/vdb /dev/sdb; do
    [ -b $d ] && mount -t iso9660 -o ro $d /mnt/cd 2>/dev/null && break 2
  done
  sleep 1; i=$((i+1))
done
mount -t squashfs -o ro,loop /mnt/cd/live/rootfs.squashfs /mnt/ro
# Persistence: if a disk is labelled AURORA_DATA, use it as the writable overlay
# upper so the whole system delta (apps, pins, settings, home) survives reboots.
# Otherwise fall back to tmpfs (ephemeral live session). Format via the shell's
# "Enable Persistent Storage" menu item (aurorad mkfs.ext4 -L AURORA_DATA).
PDEV=$(findfs LABEL=AURORA_DATA 2>/dev/null)
if [ -n "$PDEV" ] && mount -t ext4 "$PDEV" /mnt/rw 2>/dev/null; then
  echo "aurora: persistent storage on $PDEV"
else
  mount -t tmpfs none /mnt/rw
fi
mkdir -p /mnt/rw/upper /mnt/rw/work
mount -t overlay -o lowerdir=/mnt/ro,upperdir=/mnt/rw/upper,workdir=/mnt/rw/work overlay /mnt/newroot
mkdir -p /mnt/newroot/run
# The squashfs carries a disk-install /etc/fstab (/dev/sda1,2) that doesn't exist
# in the live boot — systemd would block 90s per entry then drop to emergency.
# Neutralize it in the overlay (the on-disk install keeps its real fstab).
mkdir -p /mnt/newroot/etc
printf '# live image: root is an overlay, nothing to mount from fstab\n' > /mnt/newroot/etc/fstab
umount /proc /sys
exec switch_root /mnt/newroot /usr/lib/systemd/systemd
EOF
chmod +x "$IR/init"
( cd "$IR" && find . | cpio -o -H newc | gzip -9 ) > "$WORK/iso/boot/initramfs.gz"

echo "== 3/4 grub config =="
cp "$LFS/boot/vmlinuz-aurora" "$WORK/iso/boot/"
cat > "$WORK/iso/boot/grub/grub.cfg" <<EOF
insmod search_label
insmod chain
# Hand off to an installed AuroraOS when one exists (its ESP is labelled
# "EFI" by the installer). VirtualBox EFI — and many real firmwares — insist
# on booting removable media first, so without this the attached ISO/USB
# hijacks every reboot. The live/installer entry stays in the menu for
# reinstalls; it is only the default on a blank machine.
if search --no-floppy --label EFI --set=esp; then
  set default=installed
else
  set default=live
fi
# Silent, macOS-style boot: nothing is drawn; hold/press ESC within 2s to
# open this menu (e.g. to reinstall over an existing system).
set timeout=2
set timeout_style=hidden
menuentry "AuroraOS (installed system)" --id installed {
  chainloader (\$esp)/EFI/BOOT/BOOTX64.EFI
}
menuentry "AuroraOS ${DISTRO_VERSION} — live / installer" --id live {
  linux /boot/vmlinuz-aurora quiet loglevel=3 systemd.show_status=0 udev.log_level=3 vt.global_cursor_default=0 video=1920x1080
  initrd /boot/initramfs.gz
}
EOF

echo "== 4/4 grub-mkrescue =="
# Prefer a host grub-mkrescue; otherwise use the one built into $LFS (chroot grub).
# The chroot grub only has the EFI platform, so the ISO is UEFI-boot only
# (enable EFI in the VM). xorriso + mtools must be on the host PATH.
if command -v grub-mkrescue >/dev/null; then
  grub-mkrescue -o "$OUT" "$WORK/iso"
elif [ -x "$LFS/usr/bin/grub-mkrescue" ]; then
  echo "   (using chroot-built grub-mkrescue — EFI-only ISO)"
  PATH="$LFS/usr/bin:$PATH" "$LFS/usr/bin/grub-mkrescue" \
    -d "$LFS/usr/lib/grub" -o "$OUT" "$WORK/iso"
else
  echo "!! no grub-mkrescue on host or in \$LFS"; exit 1
fi
rm -rf "$WORK"
echo "== done: $OUT — test with:"
echo "   scripts/99-smoke-qemu.sh $OUT"
