#!/bin/bash
# Asserts build.conf derives the right per-arch values. Runs on any host:
# AURORA_ARCH is forced, so this needs no aarch64 machine.
set -u
FAIL=0
chk(){ # chk <arch> <var> <expected>
  got=$(AURORA_ARCH=$1 bash -c '. "$(dirname "$0")/../config/build.conf" >/dev/null 2>&1; eval echo "\$'"$2"'"' "$0")
  if [ "$got" = "$3" ]; then echo "OK:   $1 $2=$got"
  else echo "FAIL: $1 $2='$got' (want '$3')"; FAIL=1; fi
}
chk x86_64  LFS_TGT          x86_64-lfs-linux-gnu
chk x86_64  KERNEL_IMAGE     arch/x86/boot/bzImage
chk x86_64  GRUB_TARGET      x86_64-efi
chk x86_64  LDSO             /lib64/ld-linux-x86-64.so.2
chk x86_64  FIREFOX_PLATFORM linux-x86_64
chk aarch64 LFS_TGT          aarch64-lfs-linux-gnu
chk aarch64 KERNEL_IMAGE     arch/arm64/boot/Image
chk aarch64 GRUB_TARGET      arm64-efi
chk aarch64 LDSO             /lib/ld-linux-aarch64.so.1
chk aarch64 FIREFOX_PLATFORM linux-aarch64
# unsupported arch must fail loudly
if AURORA_ARCH=mips bash -c '. "$(dirname "$0")/../config/build.conf"' "$0" 2>/dev/null; then
  echo "FAIL: unsupported arch did not error"; FAIL=1
else echo "OK:   unsupported arch rejected"; fi
[ $FAIL = 0 ] && echo "== all config checks pass ==" || exit 1
