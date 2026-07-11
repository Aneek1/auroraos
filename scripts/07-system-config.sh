#!/bin/bash
# AuroraOS 07 — system configuration + branding. Run INSIDE the chroot.
set -e
. /aurora/config/build.conf 2>/dev/null || { DISTRO_NAME=AuroraOS; DISTRO_VERSION=1.0; DISTRO_CODENAME=daybreak; DISTRO_USER=aneek; }
# build.conf sets STAMPS=$LFS/... (host path); inside the chroot it must be absolute
STAMPS=/var/lib/aurora-build

ROOTDEV=$(grep ROOT /.aurora-disk 2>/dev/null | cut -d= -f2)
ESPDEV=$(grep ESP /.aurora-disk 2>/dev/null | cut -d= -f2)

# fstab
cat > /etc/fstab <<EOF
# AuroraOS fstab
${ROOTDEV:-/dev/sda2}  /          ext4  defaults            1 1
${ESPDEV:-/dev/sda1}   /boot/efi  vfat  umask=0077          0 1
EOF

# hostname + network (systemd-networkd, DHCP on everything)
echo aurora > /etc/hostname
mkdir -p /etc/systemd/network
cat > /etc/systemd/network/10-dhcp.network <<"EOF"
[Match]
Name=en* eth*

[Network]
DHCP=yes
EOF
systemctl enable systemd-networkd systemd-resolved 2>/dev/null || true
ln -sfv /run/systemd/resolve/resolv.conf /etc/resolv.conf

# os-release + branding
cat > /etc/os-release <<EOF
NAME="$DISTRO_NAME"
VERSION="$DISTRO_VERSION ($DISTRO_CODENAME)"
ID=auroraos
PRETTY_NAME="$DISTRO_NAME $DISTRO_VERSION"
VERSION_CODENAME=$DISTRO_CODENAME
HOME_URL="https://aneek1.github.io"
EOF
echo "$DISTRO_VERSION" > /etc/lfs-release
cat > /etc/lsb-release <<EOF
DISTRIB_ID=$DISTRO_NAME
DISTRIB_RELEASE=$DISTRO_VERSION
DISTRIB_CODENAME=$DISTRO_CODENAME
DISTRIB_DESCRIPTION="$DISTRO_NAME $DISTRO_VERSION"
EOF
install -m644 /aurora/branding/issue /etc/issue
install -m644 /aurora/branding/motd  /etc/motd

# console + locale
cat > /etc/vconsole.conf <<"EOF"
KEYMAP=us
EOF
cat > /etc/locale.conf <<"EOF"
LANG=en_SG.UTF-8
EOF
cat > /etc/profile <<"EOF"
export LANG=en_SG.UTF-8
export PS1='\u@\h:\w\$ '
EOF

# users — set passwords non-interactively so the build is scriptable.
# Defaults are for the concept image; override with AURORA_ROOT_PW / AURORA_USER_PW,
# or change them on first boot.
echo "== set root password (default 'aurora' — change on first boot) =="
echo "root:${AURORA_ROOT_PW:-aurora}" | chpasswd
if ! id "$DISTRO_USER" &>/dev/null; then
  useradd -m -G wheel,audio,video,input -s /bin/bash "$DISTRO_USER"
  echo "== set password for $DISTRO_USER (default 'aurora') =="
  echo "${DISTRO_USER}:${AURORA_USER_PW:-aurora}" | chpasswd
fi
# kiosk user (no password login, no shell)
id aurora &>/dev/null || useradd -r -m -d /var/lib/aurora -G video,input,seat -s /usr/bin/false aurora

touch $STAMPS/ch9-config
echo "== 07 complete — run /aurora/scripts/08-kernel.sh =="
