#!/bin/bash
# AuroraOS 08 — Linux kernel. Run INSIDE the chroot.
set -e
. /aurora/config/build.conf
STAMPS=/var/lib/aurora-build
cd /sources
TB=$(ls linux-*.tar.xz | head -1)
SRC=${TB%.tar.xz}

if [ ! -f $STAMPS/kernel ]; then
  rm -rf $SRC; tar xf $TB; cd $SRC
  make mrproper
  make defconfig
  # merge common fragment, then the per-arch one
  scripts/kconfig/merge_config.sh -m .config /aurora/config/kernel.fragment
  scripts/kconfig/merge_config.sh -m .config /aurora/config/kernel-$AURORA_ARCH.fragment
  make olddefconfig
  make
  make modules_install
  cp -iv $KERNEL_IMAGE /boot/vmlinuz-aurora
  cp -iv System.map /boot/System.map-aurora
  cp -iv .config /boot/config-aurora
  cd /sources
  touch $STAMPS/kernel
fi
echo "== 08 complete — run /aurora/scripts/09-bootloader.sh =="
