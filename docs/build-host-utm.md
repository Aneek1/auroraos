# aarch64 build host: Debian 12 VM in UTM on the M4 MacBook Pro

LFS builds natively — to produce aarch64 AuroraOS you build *on* aarch64.
The M4 itself is the build machine, via a VM.

## One-time setup (on macOS)

1. Install UTM: https://mac.getutm.app (or `brew install --cask utm`).
2. Download Debian 12 **arm64** netinst ISO:
   https://cdimage.debian.org/debian-cd/current/arm64/iso-cd/
3. UTM → New VM → **Virtualize** (not Emulate) → Linux → pick the ISO.
   - CPU: 6 cores, RAM: 8 GB (more of both = faster; ~200 SBU total build)
   - Disk 1 (system): 40 GB
4. Install Debian (defaults fine; install "SSH server", skip desktop).
5. After install, add a **second** disk in UTM settings: VirtIO, 40 GB.
   This becomes `$LFS_DISK` and **will be wiped** by `01-prepare-disk.sh`.
   Inside the VM it appears as `/dev/vdb` — set `LFS_DISK=/dev/vdb` in
   `config/build.conf` (or export it) before running script 01.
6. Inside the VM, as root:

   ```bash
   apt update && apt install -y git build-essential m4 bison gawk texinfo \
     python3 wget curl xz-utils file bc libssl-dev libelf-dev flex cpio \
     squashfs-tools xorriso mtools grub-efi-arm64-bin dosfstools parted \
     qemu-system-arm qemu-efi-aarch64 busybox-static sudo
   ln -sf bash /bin/sh    # LFS requires /bin/sh -> bash
   git clone https://github.com/Aneek1/auroraos.git /root/auroraos
   cd /root/auroraos && bash scripts/00-check-host.sh
   ```

## Build

Run scripts 00→11 in order per README. `AURORA_ARCH` auto-detects as
`aarch64` inside the VM — no flags needed. Budget 6–12 h of compile time.

## Testing the result

- Quick: `scripts/99-smoke-qemu.sh auroraos-1.0.iso` inside the VM (nested,
  slow but proves boot).
- Real: copy the ISO to macOS and boot it in a fresh UTM VM (Virtualize,
  boot from ISO) — this is the Apple-Virtualization-adjacent environment and
  the WS-A acceptance test from the spec.
