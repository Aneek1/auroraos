# WS-A: AuroraOS aarch64 Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the AuroraOS LFS build arch-parametric so the identical scripts produce an aarch64 system that boots (with the full Aurora shell) in a VM on the M4 MacBook Pro, while x86_64 keeps working.

**Architecture:** LFS builds natively — the aarch64 build runs inside an aarch64 Debian 12 VM (UTM on the M4). All scripts read `AURORA_ARCH` (defaulting to the build host's `uname -m`) from `config/build.conf`, which derives `LFS_TGT`, the dynamic-loader path, kernel image path, GRUB target, and Firefox platform. Kernel config splits into a common fragment plus per-arch fragments. No cross-compilation from x86 to arm — the build host arch IS the target arch.

**Tech Stack:** bash (LFS 12.3 build scripts), GNU toolchain, Linux 6.13.4, GRUB (x86_64-efi / arm64-efi), QEMU + edk2 for smoke tests, UTM on macOS as the aarch64 build host.

**Spec:** `docs/superpowers/specs/2026-07-10-m4-baremetal-design.md` (this plan implements WS-A only; WS-B gets its own plan after the rig prerequisites exist).

**Testing note:** These are build-orchestration shell scripts — the test rhythm per task is: (1) `bash -n` syntax check on every edited script, (2) run `tests/test_build_conf.sh` which sources the config under both arch values and asserts derived variables, (3) commit. The end-to-end proof (Tasks 10–11) happens inside the UTM VM.

All commands below run from the repo root in Git Bash on Windows unless a task says otherwise.

---

### Task 1: Arch parametrization in `config/build.conf` + config test

**Files:**
- Modify: `config/build.conf`
- Create: `tests/test_build_conf.sh`

- [ ] **Step 1: Write the failing test**

Create `tests/test_build_conf.sh`:

```bash
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bash tests/test_build_conf.sh`
Expected: FAIL lines for every aarch64/derived variable (they don't exist yet).

- [ ] **Step 3: Implement — replace the hardcoded lines in `config/build.conf`**

Replace line `export LFS_TGT=x86_64-lfs-linux-gnu` with:

```bash
# Build-host arch IS the target arch (LFS builds natively; aarch64 builds
# run inside an aarch64 VM — see docs/build-host-utm.md)
export AURORA_ARCH=${AURORA_ARCH:-$(uname -m)}
case "$AURORA_ARCH" in
  x86_64)
    export KERNEL_IMAGE=arch/x86/boot/bzImage
    export GRUB_TARGET=x86_64-efi        # grub-install
    export LDSO=/lib64/ld-linux-x86-64.so.2
    export FIREFOX_PLATFORM=linux-x86_64
    ;;
  aarch64)
    export KERNEL_IMAGE=arch/arm64/boot/Image
    export GRUB_TARGET=arm64-efi
    export LDSO=/lib/ld-linux-aarch64.so.1
    export FIREFOX_PLATFORM=linux-aarch64
    ;;
  *) echo "!! unsupported AURORA_ARCH: $AURORA_ARCH (x86_64|aarch64)"; return 1 2>/dev/null || exit 1 ;;
esac
export LFS_TGT=${AURORA_ARCH}-lfs-linux-gnu
```

(Leave every other line of build.conf untouched.)

- [ ] **Step 4: Run test to verify it passes**

Run: `bash tests/test_build_conf.sh`
Expected: all `OK:` lines + `== all config checks pass ==`

- [ ] **Step 5: Syntax-check and commit**

```bash
bash -n config/build.conf
git add config/build.conf tests/test_build_conf.sh
git commit -m "feat(arch): parametrize build.conf for x86_64/aarch64"
```

---

### Task 2: Split kernel fragments + arch-aware `08-kernel.sh`

**Files:**
- Modify: `config/kernel.fragment` (becomes common-only)
- Create: `config/kernel-x86_64.fragment`
- Create: `config/kernel-aarch64.fragment`
- Modify: `scripts/08-kernel.sh`

- [ ] **Step 1: Rewrite `config/kernel.fragment` (arch-neutral common config)**

```
# AuroraOS kernel fragment (COMMON, all arches) — merged over defconfig,
# then config/kernel-$AURORA_ARCH.fragment is merged on top.
# EFI boot
CONFIG_EFI=y
CONFIG_EFI_STUB=y
CONFIG_EFIVAR_FS=y
# systemd requirements
CONFIG_CGROUPS=y
CONFIG_INOTIFY_USER=y
CONFIG_SECCOMP=y
CONFIG_TMPFS=y
CONFIG_TMPFS_POSIX_ACL=y
CONFIG_TMPFS_XATTR=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y
CONFIG_FHANDLE=y
CONFIG_AUTOFS_FS=y
CONFIG_DMIID=y
CONFIG_NET_NS=y
CONFIG_USER_NS=y
CONFIG_SIGNALFD=y
CONFIG_TIMERFD=y
CONFIG_EPOLL=y
CONFIG_UNIX=y
CONFIG_PROC_FS=y
CONFIG_SYSFS=y
# graphics common (VM + firmware framebuffer)
CONFIG_DRM=y
CONFIG_DRM_FBDEV_EMULATION=y
CONFIG_DRM_VIRTIO_GPU=y
CONFIG_DRM_SIMPLEDRM=y
CONFIG_FRAMEBUFFER_CONSOLE=y
# input
CONFIG_INPUT_EVDEV=y
CONFIG_INPUT_MOUSEDEV=y
CONFIG_HID_GENERIC=y
CONFIG_USB_HID=y
CONFIG_USB_XHCI_HCD=y
CONFIG_USB_EHCI_HCD=y
# virtio (QEMU/UTM/Apple Virtualization guests)
CONFIG_VIRTIO=y
CONFIG_VIRTIO_PCI=y
CONFIG_VIRTIO_MMIO=y
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_NET=y
CONFIG_VIRTIO_CONSOLE=y
CONFIG_VIRTIO_BALLOON=y
CONFIG_VIRTIO_INPUT=y
# virtio-scsi cdrom (live ISO in VMs)
CONFIG_SCSI=y
CONFIG_SCSI_VIRTIO=y
CONFIG_BLK_DEV_SD=y
CONFIG_BLK_DEV_SR=y
# storage + fs
CONFIG_SATA_AHCI=y
CONFIG_NVME_CORE=y
CONFIG_BLK_DEV_NVME=y
CONFIG_EXT4_FS=y
CONFIG_VFAT_FS=y
CONFIG_FAT_DEFAULT_UTF8=y
CONFIG_NLS_CODEPAGE_437=y
CONFIG_NLS_ISO8859_1=y
# live ISO support
CONFIG_SQUASHFS=y
CONFIG_SQUASHFS_XZ=y
CONFIG_SQUASHFS_ZSTD=y
CONFIG_OVERLAY_FS=y
CONFIG_ISO9660_FS=y
CONFIG_BLK_DEV_LOOP=y
# backlight control for aurorad
CONFIG_BACKLIGHT_CLASS_DEVICE=y
```

- [ ] **Step 2: Create `config/kernel-x86_64.fragment`** (the x86-only lines removed from common)

```
# AuroraOS x86_64 fragment — real PC hardware + x86 VMs
CONFIG_FB_EFI=y
CONFIG_DRM_I915=y
CONFIG_DRM_AMDGPU=y
CONFIG_DRM_QXL=y
CONFIG_DRM_BOCHS=y
# network
CONFIG_E1000=y
CONFIG_E1000E=y
CONFIG_R8169=y
CONFIG_IWLWIFI=m
CONFIG_CFG80211=m
CONFIG_MAC80211=m
# ACPI backlight
CONFIG_ACPI_VIDEO=y
# sound (basic HDA)
CONFIG_SOUND=y
CONFIG_SND=y
CONFIG_SND_HDA_INTEL=y
```

- [ ] **Step 3: Create `config/kernel-aarch64.fragment`**

```
# AuroraOS aarch64 fragment — QEMU virt / UTM / Apple Virtualization.
# arm64 defconfig already enables PL011, virt platform, PCIe host generic;
# these are belt-and-braces + VM niceties.
CONFIG_PCI_HOST_GENERIC=y
CONFIG_SERIAL_AMBA_PL011=y
CONFIG_SERIAL_AMBA_PL011_CONSOLE=y
CONFIG_RTC_DRV_PL031=y
CONFIG_ARM_SCMI_PROTOCOL=y
# Apple VZ console + entropy
CONFIG_HW_RANDOM_VIRTIO=y
```

- [ ] **Step 4: Rewrite `scripts/08-kernel.sh`** (source config, merge both fragments, arch image path)

```bash
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
```

Note: `. /aurora/config/build.conf` works because `05-enter-chroot.sh` bind-mounts the repo at `/aurora` (verify while editing — if the mount point differs, match it). Inside the chroot `uname -m` reports the VM's arch, so `AURORA_ARCH` auto-derives correctly.

- [ ] **Step 5: Syntax check + fragment sanity + commit**

```bash
bash -n scripts/08-kernel.sh
# no x86-only symbols left in common fragment:
grep -E 'I915|AMDGPU|E1000|HDA_INTEL|QXL|BOCHS|IWLWIFI|ACPI_VIDEO|FB_EFI' config/kernel.fragment && echo "LEAK" || echo "common fragment clean"
git add config/kernel*.fragment scripts/08-kernel.sh
git commit -m "feat(arch): split kernel config into common + per-arch fragments"
```

Expected: `common fragment clean`.

---

### Task 3: Arch-aware toolchain pass 1 (`scripts/03-toolchain-pass1.sh`)

**Files:**
- Modify: `scripts/03-toolchain-pass1.sh`

Three x86-isms: the `lib64` scaffolding, the glibc loader symlinks (lines 69–70), and the glibc sanity grep (line 81). The gcc `t-linux64` sed (line 42) is already guarded by `case $(uname -m)` — leave it.

- [ ] **Step 1: Pass the new variables into the clean `sudo -u lfs env -i` environment**

Line 15–17 currently passes `LFS LFS_TGT MAKEFLAGS ...`. Change to also pass `AURORA_ARCH` and `LDSO`:

```bash
sudo -u lfs env -i HOME=/home/lfs TERM="$TERM" \
  LFS="$LFS" LFS_TGT="$LFS_TGT" MAKEFLAGS="$MAKEFLAGS" \
  AURORA_ARCH="$AURORA_ARCH" LDSO="$LDSO" \
  PATH="$LFS/tools/bin:/usr/bin:/bin" CONFIG_SITE="$LFS/usr/share/config.site" \
  bash -e <<'LFSEOF'
```

- [ ] **Step 2: Guard the lib64 dir creation (line 10)**

Replace:
```bash
mkdir -pv "$LFS"/{etc,var,usr/{bin,lib,sbin},lib64,tools}
```
with:
```bash
mkdir -pv "$LFS"/{etc,var,usr/{bin,lib,sbin},tools}
case "$AURORA_ARCH" in x86_64) mkdir -pv "$LFS/lib64" ;; esac
```
And on line 12 change `"$LFS"/{usr,lib64,tools,var,etc,sources}` to `"$LFS"/{usr,tools,var,etc,sources} "$LFS/lib64"` keeping the trailing `2>/dev/null || true` so a missing lib64 on aarch64 is harmless.

- [ ] **Step 3: Arch-conditional glibc loader symlinks (replace lines 69–70)**

```bash
  case "$AURORA_ARCH" in
    x86_64)
      ln -sfv ../lib/ld-linux-x86-64.so.2 $LFS/lib64
      ln -sfv ../lib/ld-linux-x86-64.so.2 $LFS/lib64/ld-lsb-x86-64.so.3 ;;
    aarch64)
      ln -sfv ../lib/ld-linux-aarch64.so.1 $LFS/lib64 2>/dev/null || true ;;
  esac
```

(LFS's ARM adaptation: aarch64 binaries reference `/lib/ld-linux-aarch64.so.1`, which resolves via the `lib → usr/lib` symlink; no lib64 needed.)

- [ ] **Step 4: Arch-agnostic sanity check (replace line 81)**

```bash
  readelf -l a.out | grep -q "$LDSO" || { echo "!! glibc sanity FAILED (want $LDSO)"; exit 1; }
```

- [ ] **Step 5: Verify and commit**

```bash
bash -n scripts/03-toolchain-pass1.sh
bash tests/test_build_conf.sh
git add scripts/03-toolchain-pass1.sh
git commit -m "feat(arch): make toolchain pass 1 arch-aware (loader symlinks, sanity, lib64)"
```

---

### Task 4: Arch-aware temp tools (`scripts/04-temp-tools.sh`)

**Files:**
- Modify: `scripts/04-temp-tools.sh:13`

- [ ] **Step 1: Fix the config.guess fallback**

Line 13, replace:
```bash
CFG="--host=$LFS_TGT --build=$(sh gcc-*/config.guess 2>/dev/null || echo x86_64-pc-linux-gnu)"
```
with:
```bash
CFG="--host=$LFS_TGT --build=$(sh gcc-*/config.guess 2>/dev/null || echo $(uname -m)-pc-linux-gnu)"
```
(Line 82's gcc sed is already `case $(uname -m)`-guarded — no change.)

- [ ] **Step 2: Verify and commit**

```bash
bash -n scripts/04-temp-tools.sh
git add scripts/04-temp-tools.sh
git commit -m "feat(arch): arch-neutral config.guess fallback in temp tools"
```

---

### Task 5: Arch-aware GRUB package build (`scripts/06-base-system.sh`)

**Files:**
- Modify: `scripts/06-base-system.sh:283-285`

- [ ] **Step 1: Make the grub recipe use the arch target**

The grub recipe currently reads (lines 283–287):
```bash
    grub)      unset {C,CPP,CXX,LD}FLAGS
               ./configure --prefix=/usr --sysconfdir=/etc --disable-efiemu --disable-werror \
                 --with-platform=efi --target=x86_64 --program-prefix=""
               make; make install
               mv -v /etc/bash_completion.d/grub /usr/share/bash-completion/completions 2>/dev/null||true;;
```
It is a plain (unquoted) `case` branch, so variables expand normally. Replace the branch with:
```bash
    grub)      unset {C,CPP,CXX,LD}FLAGS
               case $(uname -m) in aarch64) GT=arm64;; *) GT=x86_64;; esac
               ./configure --prefix=/usr --sysconfdir=/etc --disable-efiemu --disable-werror \
                 --with-platform=efi --target=$GT --program-prefix=""
               make; make install
               mv -v /etc/bash_completion.d/grub /usr/share/bash-completion/completions 2>/dev/null||true;;
```

(Line 210's gcc sanity grep `': /usr/lib/ld-linux'` already matches both arches' loaders — no change. Line 356's `$(uname -m)-lfs-linux-gnu` cleanup is already arch-neutral — no change.)

- [ ] **Step 2: Verify and commit**

```bash
bash -n scripts/06-base-system.sh
git add scripts/06-base-system.sh
git commit -m "feat(arch): build grub for arm64-efi on aarch64"
```

---

### Task 6: Arch-aware Firefox download (`scripts/02-download-sources.sh`)

**Files:**
- Modify: `scripts/02-download-sources.sh:26-27`

- [ ] **Step 1: Use `$FIREFOX_PLATFORM` and verify availability before downloading**

Replace lines 26–27 with:

```bash
FF_URL="https://download-installer.cdn.mozilla.net/pub/firefox/releases/${FIREFOX_VERSION}/${FIREFOX_PLATFORM}/en-US/firefox-${FIREFOX_VERSION}.tar.xz"
if curl -sfIL "$FF_URL" >/dev/null; then
  wget -nc "$FF_URL"
else
  echo "!! No Firefox ${FIREFOX_VERSION} build for ${FIREFOX_PLATFORM} on Mozilla CDN."
  echo "!! Check https://ftp.mozilla.org/pub/firefox/releases/ for a version with a"
  echo "!! ${FIREFOX_PLATFORM} directory and set FIREFOX_VERSION in config/build.conf."
  exit 1
fi
```

- [ ] **Step 2: Verify the aarch64 URL actually exists for the pinned version (from this machine, now)**

Run: `curl -sfIL "https://download-installer.cdn.mozilla.net/pub/firefox/releases/128.8.0esr/linux-aarch64/en-US/firefox-128.8.0esr.tar.xz" >/dev/null && echo EXISTS || echo MISSING`

If MISSING: browse `https://ftp.mozilla.org/pub/firefox/releases/` for the nearest version that ships `linux-aarch64` (Mozilla added official Linux ARM64 tarballs in the Fx 128 era; some point releases lack them) and update `FIREFOX_VERSION` in `config/build.conf` accordingly, noting in the commit message that both platforms were verified.

- [ ] **Step 3: Verify both platform URLs and commit**

```bash
bash -n scripts/02-download-sources.sh
. config/build.conf 2>/dev/null || true
for p in linux-x86_64 linux-aarch64; do
  curl -sfIL "https://download-installer.cdn.mozilla.net/pub/firefox/releases/${FIREFOX_VERSION}/${p}/en-US/firefox-${FIREFOX_VERSION}.tar.xz" >/dev/null && echo "OK $p" || echo "MISSING $p"
done
git add scripts/02-download-sources.sh config/build.conf
git commit -m "feat(arch): arch-aware Firefox download with availability check"
```

Expected: `OK linux-x86_64` and `OK linux-aarch64` before committing.

---

### Task 7: Arch-aware bootloader install (`scripts/09-bootloader.sh`)

**Files:**
- Modify: `scripts/09-bootloader.sh`

- [ ] **Step 1: Source the config and use `$GRUB_TARGET`**

After line 3 (`set -e`) add:
```bash
. /aurora/config/build.conf
```
Replace both `--target=x86_64-efi` occurrences (lines 9 and 11) with `--target=$GRUB_TARGET`.

The grub.cfg body is arch-neutral (GRUB's `linux` command loads both bzImage and arm64 Image on EFI) — no change.

- [ ] **Step 2: Verify and commit**

```bash
bash -n scripts/09-bootloader.sh
git add scripts/09-bootloader.sh
git commit -m "feat(arch): grub-install uses per-arch EFI target"
```

---

### Task 8: Arch-aware live ISO (`scripts/11-make-iso.sh`) + QEMU smoke script

**Files:**
- Modify: `scripts/11-make-iso.sh`
- Create: `scripts/99-smoke-qemu.sh`

- [ ] **Step 1: Update host-deps comment and QEMU hint in `11-make-iso.sh`**

Line 3 comment becomes:
```bash
# Needs on host: squashfs-tools, xorriso, cpio, mtools, and grub EFI tools for
# the build arch (x86_64: grub-efi-amd64-bin + grub-pc-bin; aarch64: grub-efi-arm64-bin).
```
Replace the final echo (line 60) with:
```bash
echo "   scripts/99-smoke-qemu.sh $OUT"
```
`grub-mkrescue` itself auto-detects installed GRUB platform dirs, so the invocation on line 57 is unchanged; on aarch64 it produces an EFI-only ISO (no BIOS boot — correct, arm64 has no BIOS).

- [ ] **Step 2: Create `scripts/99-smoke-qemu.sh`**

```bash
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
```

- [ ] **Step 3: Verify and commit**

```bash
bash -n scripts/11-make-iso.sh scripts/99-smoke-qemu.sh
git update-index --chmod=+x scripts/99-smoke-qemu.sh 2>/dev/null || true
git add scripts/11-make-iso.sh scripts/99-smoke-qemu.sh
git commit -m "feat(arch): arch-aware live ISO + QEMU smoke-test script"
```

---

### Task 9: Build-host doc (UTM on the M4) + README update

**Files:**
- Create: `docs/build-host-utm.md`
- Modify: `README.md`

- [ ] **Step 1: Write `docs/build-host-utm.md`**

```markdown
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
   apt update && apt install -y git build-essential bison gawk texinfo \
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
```

- [ ] **Step 2: Update `README.md`**

In the Requirements section, after the build-host bullet, add:

```markdown
- **Architectures:** x86_64 and aarch64. The build host's arch is the target
  arch (`AURORA_ARCH` auto-detects). For aarch64 — including running AuroraOS
  on Apple Silicon in a VM — see `docs/build-host-utm.md`.
```

And add a final section:

```markdown
## Roadmap: Apple Silicon bare metal

The aarch64 port is workstream A of the M4 bare-metal plan — see
`docs/superpowers/specs/2026-07-10-m4-baremetal-design.md`. Bare-metal M4
(m1n1/t8132 bringup) is workstream B and has its own milestone ladder.
```

- [ ] **Step 3: Commit**

```bash
git add docs/build-host-utm.md README.md
git commit -m "docs: aarch64 build-host guide (UTM on M4) + README arch notes"
```

---

### Task 10: Repo-wide review pass + push

**Files:**
- Verify all previous edits; push to GitHub.

- [ ] **Step 1: Full syntax sweep + residual-x86 audit**

```bash
for f in scripts/*.sh; do bash -n "$f" || echo "SYNTAX FAIL: $f"; done
bash tests/test_build_conf.sh
# any remaining hardcoded x86-isms outside the deliberately-guarded lines?
grep -rn 'x86_64\|bzImage\|amd64' scripts/ config/ | grep -v 'case\|AURORA_ARCH\|kernel-x86_64\|uname'
```
Expected: no syntax failures, config test passes, and the grep returns only intentional per-arch lines (review each hit; fix any stragglers).

- [ ] **Step 2: Check `.gitattributes` keeps scripts LF** (repo was authored on Windows)

```bash
cat .gitattributes   # expect: * text=auto eol=lf or *.sh eol=lf; add "*.sh text eol=lf" if absent
git ls-files --eol scripts/ | grep -v 'w/lf' && echo "CRLF LEAK" || echo "line endings OK"
```

- [ ] **Step 3: Push**

```bash
git push origin master
```

---

### Task 11: End-to-end aarch64 build + acceptance (runs on the M4, user-driven)

This task executes on the M4, not this PC. It is the WS-A acceptance test from the spec: **AuroraOS boots in a VM on the M4 with the full Aurora shell.**

- [ ] **Step 1:** Follow `docs/build-host-utm.md` one-time setup (UTM + Debian arm64 VM + packages + clone).
- [ ] **Step 2:** Run scripts 00→02. Checkpoint: `00` prints `Host OK`; `02` ends with all sources downloaded incl. `firefox-*.tar.xz` (aarch64).
- [ ] **Step 3:** Run scripts 03→04 (~2 h). Checkpoint: `03` prints `glibc done, sanity OK` — the readelf check against `/lib/ld-linux-aarch64.so.1` is the first real proof the arch port works.
- [ ] **Step 4:** Run 05→07 (3–6 h for 06). Checkpoint: `06` completes all recipes; if a package fails, fix per the LFS 12.3 book section named in the error, re-run (stamps resume).
- [ ] **Step 5:** Run 08→09. Checkpoint: `/boot/vmlinuz-aurora` exists and `file` reports `ARM64 kernel image`; `grub-install` reports no errors for `arm64-efi`.
- [ ] **Step 6:** Run 10, then 11. Checkpoint: `auroraos-1.0.iso` produced.
- [ ] **Step 7:** Acceptance: boot the ISO in a fresh UTM VM on macOS. PASS = GRUB menu → kernel boots → `cage` + Firefox kiosk shows the Aurora shell, clock ticks, `aurorad` responds (battery tile shows "no battery" gracefully in a VM). Photograph/screenshot for the milestone log, per the spec's verification rule.
- [ ] **Step 8:** Commit any fixes discovered during the build from the VM (`git config` the VM, push to a branch, merge here), tag `v1.1-aarch64`.

---

## Deferred (explicitly not in this plan)

- WS-B (m1n1/t8132, RE rig, WSL2+usbipd, stub APFS volume) — own plan after this ships and the rig prerequisites from the spec checklist are met.
- GPU acceleration, speakers, webcam — out of scope per spec.
- x86_64 boot-anywhere driver expansion — separate future project (spec: out of scope).
