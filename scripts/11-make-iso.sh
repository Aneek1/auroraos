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
mount -t tmpfs none /mnt/rw
mkdir -p /mnt/rw/upper /mnt/rw/work
mount -t overlay -o lowerdir=/mnt/ro,upperdir=/mnt/rw/upper,workdir=/mnt/rw/work overlay /mnt/newroot
mkdir -p /mnt/newroot/run
umount /proc /sys
exec switch_root /mnt/newroot /usr/lib/systemd/systemd
EOF
chmod +x "$IR/init"
( cd "$IR" && find . | cpio -o -H newc | gzip -9 ) > "$WORK/iso/boot/initramfs.gz"

echo "== 3/4 grub config =="
cp "$LFS/boot/vmlinuz-aurora" "$WORK/iso/boot/"
cat > "$WORK/iso/boot/grub/grub.cfg" <<EOF
set default=0
set timeout=3
menuentry "AuroraOS ${DISTRO_VERSION} — live" {
  linux /boot/vmlinuz-aurora quiet loglevel=3 vt.global_cursor_default=0
  initrd /boot/initramfs.gz
}
EOF

echo "== 4/4 grub-mkrescue =="
grub-mkrescue -o "$OUT" "$WORK/iso"
rm -rf "$WORK"
echo "== done: $OUT — test with:"
echo "   scripts/99-smoke-qemu.sh $OUT"
