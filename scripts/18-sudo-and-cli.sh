#!/bin/bash
# DaybreakOS 18 — sudo + the `daybreak` package CLI. Run INSIDE the chroot.
# sudo: built from source (no PAM in the image; auth is bypassed by the
# NOPASSWD grant below — the autologin kiosk user has no password to type).
# daybreak: terminal front-end for the Daybreak Store engine in aurorad
# (GET /store/catalog, POST /store/install|remove) — install shell/daybreak
# to /usr/bin/daybreak (755). There is deliberately NO apt/dpkg: Debian
# packages target Debian's glibc/layout and would overwrite the LFS base.
set -e
cd /sources
tar xf sudo-1.9.16p2.tar.gz
cd sudo-1.9.16p2
./configure --prefix=/usr --libexecdir=/usr/lib \
            --with-secure-path --with-env-editor \
            --docdir=/usr/share/doc/sudo-1.9.16p2
make -j"$(nproc)"
make install
cd /sources && rm -rf sudo-1.9.16p2

mkdir -p /etc/sudoers.d
printf 'aurora ALL=(ALL:ALL) NOPASSWD: ALL\n' > /etc/sudoers.d/aurora
chmod 440 /etc/sudoers.d/aurora
grep -q 'includedir /etc/sudoers.d' /etc/sudoers || \
  printf '@includedir /etc/sudoers.d\n' >> /etc/sudoers
visudo -c

echo "== 18 complete — sudo + daybreak CLI. Re-squash + rebuild ISO. =="
