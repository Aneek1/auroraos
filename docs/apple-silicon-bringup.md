# DaybreakOS on Apple Silicon — bring-up plan (M2 target, M4 experimental)

Status: PLAN (2026-07-17). Research-backed; targets a borrowed **M2** as the real
bring-up and an **M4** as a reverse-engineering experiment only. Owner hardware:
one M2 + one M4 (either can DFU-restore the other).

## 0. Reality check — what is and isn't possible today

Per the Asahi project's own feature matrix (July 2026):

| Tier | Boot (m1n1→U-Boot) | GPU accel | Installer | For us |
|------|--------------------|-----------|-----------|--------|
| **M1 / M2** (G13/G14) | ✅ mature | ✅ GL 4.6 / Vulkan 1.3 | ✅ | **REAL target** |
| M3 (G16) | ⚠️ local patches | ❌ swrast only | ❌ | n/a |
| **M4** (G16, T8132) | ❌ m1n1 broken (SPTM/GL2/EL2) | ❌ none | ❌ | **RE experiment only** |

- Apple GPU firmware is **proprietary, extracted from the machine's own macOS**, not
  redistributable, not in `linux-firmware`. We can ship the *mechanism*, never the blobs.
- The install is **non-destructive** (shrinks the macOS APFS container). The immutable
  SecureROM guarantees a DFU restore always returns the Mac to factory macOS.
- **M4 cannot boot Linux at all yet** — do not expect to run DaybreakOS on it. M4 work =
  contributing to Asahi's m1n1 SPTM boot RE, not integration.

## 1. Safety & recovery (do this BEFORE touching either Mac)

1. **Time Machine** back up both Macs (restores your macOS data — NOT the Linux partition).
2. Confirm the **DFU cross-restore rig**: USB-C cable between the two Macs; install
   **Apple Configurator** (or `idevicerestore`/libimobiledevice) on the *helper* Mac.
   Practice entering DFU on the target once so it's familiar.
3. Note: worst case = DFU restore (wipes the target's drive) → reinstall macOS → Time
   Machine restore. No permanent brick is possible; iBoot1/SecureROM are immutable.

## 2. Build environment (host side, no Mac needed)

Apple Silicon is **aarch64**. All of this builds in the existing aarch64 LFS track
(`aurora-arm64` container / WS-A aarch64 plan), NOT the x86_64 tree. Nothing here needs a
Mac — it is all build-blind until we boot on the M2.

- Base aarch64 DaybreakOS userland (existing aarch64 build scripts).
- **Mesa ≥ 25.2** with `-D gallium-drivers=asahi -D vulkan-drivers=asahi` and **LLVM ≥ 18**
  (we already build LLVM 18.1.8). NOTE: stock `gallium-drivers=auto` does NOT pull in
  asahi/honeykrisp — they must be named explicitly. (New `scripts/2x-asahi.sh`, aarch64.)
- `asahi-scripts` (fwextract/fwupdate) ported into the image — the reusable glue every
  non-Fedora distro (Debian/Gentoo/Guix) adopts.

## 3. Kernel (Asahi tree — NOT mainline)

- Source: `github.com/AsahiLinux/linux`, branch **`gpu/rust-wip`** (carries `drivers/gpu/
  drm/asahi`, the Rust GPU driver). Mainline boots M1/M2 CPU + basic peripherals but gives
  **no GPU** — must use the Asahi tree for acceleration.
- Build **with Rust-for-Linux enabled** + Asahi's out-of-tree DRM Rust abstractions.
- Device trees: `arch/arm64/boot/dts/apple/` from the Asahi tree; m1n1 also synthesizes/
  patches the FDT from Apple's ADT at boot.
- Config: enable `DRM_ASAHI` (+ its selects), Apple SoC platform drivers (DART, PCIe,
  DCP display, SMC, NVMe-Apple, brcmfmac Wi-Fi, etc.).

## 4. Boot chain + install (on the M2, from macOS)

```
SecureROM → iBoot1 → iBoot2 (Preboot) → m1n1 → U-Boot → GRUB → Linux
```

- **m1n1** (`AsahiLinux/m1n1`, ≥1.6.0 — needs Rust for stage-2) occupies iBoot's "OS kernel"
  slot, sets up hardware, ADT→FDT, chainloads U-Boot.
- **U-Boot** (`AsahiLinux/u-boot`) provides UEFI + USB/NVMe/framebuffer so a standard EFI
  bootloader works.
- **GRUB** (our existing EFI stage) loads the kernel + initramfs (incl. the vendorfw cpio).
- **Install** runs entirely on the M2 from macOS Terminal, then reboots into **1TR (One True
  recoveryOS)** — hold power — which grants the SEP privilege to create a **custom OS Boot
  Policy** set to **Permissive Security** (unsigned kernels allowed). A minimal **stub macOS**
  is installed purely to own that Boot Policy.
- Reuse the asahi-installer's staged m1n1+U-Boot images, or build from the repos. For a
  custom distro, the installer/boot-policy plumbing is identical; only the rootfs payload
  differs.

## 5. Firmware (per-boot, from the Mac's macOS)

- Run **`asahi-fwextract`** → produces `manifest.txt` + `firmware.cpio`/`firmware.tar` from
  `/boot/efi/asahi/all_firmware.tar.gz` (the installer drops this from the macOS image).
- Package as **`vendorfw.cpio`** in the ESP; bootloader/initramfs loads it as an **extra
  initramfs cpio**, unpacked to a **tmpfs at `/lib/firmware/vendor`** every boot. Keeps
  non-redistributable blobs out of the rootfs (backups stay clean) and dodges udev races.
- `asahi-fwupdate` refreshes it without returning to macOS.

## 6. GPU userspace

- M2: Mesa **asahi** gallium (OpenGL 4.6 / ES 3.2) + **honeykrisp** Vulkan 1.3 → real
  hardware acceleration once §3–§5 are in place.
- Verify: `glxinfo | grep renderer` shows the Apple GPU; `vulkaninfo` lists honeykrisp.

## 7. M4 experimental track (separate, no DaybreakOS boot expected)

- M4 = **T8132**, GPU **G16** (ray tracing / mesh shaders / Dynamic Caching).
- Hard blocker: **m1n1 broken by SPTM** (Secure Page Table Monitor at GL2; must talk to
  SPTM from EL2 with MMU already on). No stable boot, no ETA.
- Even if booted, **G16 GPU is un-reverse-engineered** — M3 (same GPU gen) is still
  software-rendered.
- Realistic M4 activity: follow Asahi progress reports; help RE the m1n1 SPTM/EL2 boot path
  and DART/PCIe. Treat the M4 purely as an RE bench, with DFU-restore-from-M2 as the reset.

## 8. Sequencing

1. **Now (no Mac):** `scripts/2x-asahi.sh` — build-blind Mesa asahi/honeykrisp on aarch64;
   confirm it compiles and installs into an aarch64 image. (Mirrors our x86 HW-GL work.)
2. **Now (no Mac):** aarch64 Asahi kernel build (Rust-enabled) + `asahi-scripts` port.
3. **With the M2:** Time Machine + DFU rig → asahi-installer boot-policy/stub-macOS → stage
   m1n1+U-Boot → install DaybreakOS aarch64 rootfs → extract firmware → boot → verify GPU.
4. **With the M4 (optional):** RE bench only; do not expect a booting system.

## Key references

- Feature matrix: https://asahilinux.org/docs/platform/feature-support/m4/
- Platform/boot: https://asahilinux.org/docs/platform/introduction/
- Progress 6.19 / 7.1: https://asahilinux.org/2026/02/progress-report-6-19/ , https://asahilinux.org/2026/06/progress-report-7-1/
- drm/asahi (rust-wip): https://github.com/AsahiLinux/linux/tree/gpu/rust-wip/drivers/gpu/drm/asahi
- Mesa 25.2 relnotes (asahi/honeykrisp upstreamed): https://docs.mesa3d.org/relnotes/25.2.0.html
- asahi-scripts (fwextract): https://github.com/AsahiLinux/asahi-scripts
