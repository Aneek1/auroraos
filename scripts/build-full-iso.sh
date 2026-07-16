#!/bin/bash
# Install the full-featured shell + assets into the chroot, then re-squash and
# reassemble the EFI ISO (librsvg/certs already in the tree).
set -e
LFS=/mnt/lfs
STAGE="$LFS/tmp/isobuild"; ISO="$STAGE/iso"
OUT=/aurora/daybreakos-1.0-desktop-full.iso
CENV="/usr/bin/env -i PATH=/tmp/isotools/bin:/usr/bin:/bin LD_LIBRARY_PATH=/tmp/isotools/lib MTOOLS_SKIP_CHECK=1"

echo "START $(date)"
echo "== install shell + style + catalog =="
install -Dm644 /aurora/shell/aurora-desktop/style.css /mnt/lfs/usr/share/aurora/desktop/style.css
install -Dm644 /aurora/store/catalog /mnt/lfs/usr/share/aurora/store/catalog
cp /aurora/shell/aurora-desktop/aurora-shell.c /mnt/lfs/tmp/aurora-shell.c
cp /aurora/shell/aurora-desktop/protocols/wlr-foreign-toplevel-management-unstable-v1.xml /mnt/lfs/tmp/ftl.xml
chroot "$LFS" /usr/bin/env -i PATH=/usr/bin:/bin /bin/bash -c \
  'wayland-scanner client-header /tmp/ftl.xml /tmp/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h && \
   wayland-scanner private-code   /tmp/ftl.xml /tmp/wlr-foreign-toplevel-management-unstable-v1-protocol.c && \
   cc /tmp/aurora-shell.c /tmp/wlr-foreign-toplevel-management-unstable-v1-protocol.c -I/tmp -O2 -o /usr/bin/aurora-shell $(pkg-config --cflags --libs gtk+-3.0 gtk-layer-shell-0 wayland-client) -lm && echo "shell: $(stat -c %s /usr/bin/aurora-shell) bytes"'

echo "== labwc autostart =="
# aurora-shell self-selects installer-vs-desktop from the root filesystem
# type (overlay/tmpfs live -> installer, ext4 disk -> desktop), so the
# autostart is plain unconditional lines.
rm -f /mnt/lfs/usr/bin/aurora-shell-select /mnt/lfs/etc/aurora-installed
install -d /mnt/lfs/etc/xdg/labwc
cat > /mnt/lfs/etc/xdg/labwc/autostart <<'EOF'
# DaybreakOS session autostart (labwc)
# push DISPLAY (set by labwc's Xwayland) into the D-Bus activation env for portals
( sleep 2; dbus-update-activation-environment --all 2>/dev/null || dbus-update-activation-environment DISPLAY WAYLAND_DISPLAY XDG_CURRENT_DESKTOP 2>/dev/null ) &
/usr/lib/aurora/aura-llm-launch &
/usr/bin/python3 /usr/lib/aurora/aurorad &
# launch the shell with DISPLAY=:0 so X11 apps it starts (Steam) reach Xwayland;
# labwc itself must NOT have DISPLAY set (breaks wlroots backend autodetect)
env DISPLAY=:0 /usr/bin/aurora-shell &
EOF

echo "== re-squash (excluding the LLM model + build toolchain to slim the ISO) =="
# The model is downloaded post-install (Aura setup); the compiler/build tools
# aren't needed at runtime. Excluded from the squashfs, NOT deleted from $LFS,
# so the shell can still be rebuilt here.
mksquashfs "$LFS" "$ISO/live/rootfs.squashfs" -comp zstd -noappend \
  -e boot/efi -e sources -e proc -e sys -e dev -e run -e tmp -e aurora \
  -e opt/aura/models -e usr/libexec/gcc -e usr/bin/lto-dump -e opt/cmake \
  -e usr/include -wildcards -e 'core.*'

echo "== grub-mkimage + ESP + xorriso =="
cat > "$STAGE/embed.cfg" <<'EOF'
search --no-floppy --set=root --file /boot/grub/grub.cfg
set prefix=($root)/boot/grub
configfile /boot/grub/grub.cfg
EOF
chroot "$LFS" /usr/bin/env -i PATH=/usr/bin:/bin \
  grub-mkimage -O x86_64-efi -p /boot/grub -c /tmp/isobuild/embed.cfg -o /tmp/isobuild/bootx64.efi \
  normal linux boot configfile search search_fs_file search_label chain iso9660 fat part_gpt part_msdos all_video efi_gop efi_uga terminal echo test sleep ls cat halt reboot
rm -f "$STAGE/efi.img"; truncate -s 16M "$STAGE/efi.img"
chroot "$LFS" $CENV mformat -i /tmp/isobuild/efi.img ::
chroot "$LFS" $CENV mmd -i /tmp/isobuild/efi.img ::/EFI ::/EFI/BOOT
chroot "$LFS" $CENV mcopy -i /tmp/isobuild/efi.img /tmp/isobuild/bootx64.efi ::/EFI/BOOT/BOOTX64.EFI
cp "$STAGE/efi.img" "$ISO/boot/efi.img"; mkdir -p "$ISO/EFI/BOOT"; cp "$STAGE/bootx64.efi" "$ISO/EFI/BOOT/BOOTX64.EFI"
chroot "$LFS" $CENV xorriso -as mkisofs -o /tmp/isobuild/out.iso -V DAYBREAKOS -e boot/efi.img -no-emul-boot /tmp/isobuild/iso
cp "$STAGE/out.iso" "$OUT"
echo "END rc=$? $(date)"; ls -la "$OUT"
