# AuroraOS on Apple Silicon M4 — bare-metal dual-boot design

**Date:** 2026-07-10
**Status:** Approved (design), plan pending
**Target hardware:** MacBook Pro (M4 family — t8132-class SoC), dual-booting macOS + AuroraOS
**Prior state:** AuroraOS is an x86_64 LFS 12.3 build (scripts 00–11) booting a Wayland-kiosk web shell. No aarch64 support, no Mac support.

## Goal

Boot AuroraOS bare-metal on the user's M4 MacBook Pro alongside macOS, Asahi-Linux-style: macOS stays intact and fully secure; AuroraOS lives in its own APFS volume with per-volume Permissive Security. Where the M4 SoC is unsupported upstream (it currently is — Asahi covers M1/M2 only), we do the bringup ourselves, standing on Asahi's architecture (m1n1, their kernel tree, their installer) rather than rewriting it.

## Accepted realities (agreed with user)

- M4 (t8132) bare-metal Linux **does not exist anywhere yet**. This is a research project with a milestone ladder, not a scripted build. Timescale for a usable bare-metal boot is months, not days.
- GPU acceleration is **out of scope** for the foreseeable future. The Aurora shell runs on software rendering (llvmpipe) once display works.
- The user's main computer is this Windows work PC; there is **no second Mac**. Worst-case recovery (a wedged boot object requiring DFU restore) needs another Mac running Apple Configurator — accepted risk, mitigated by: full backup before partitioning, never touching the macOS volume's boot policy, and the fact that deleting the Aurora APFS volume from recoveryOS undoes everything in the normal case.

## Architecture

### Boot chain (Apple-sanctioned custom-kernel path)

```
SecureROM → iBoot1 → iBoot2 (Aurora stub volume, Permissive Security)
  → m1n1 (our fork, + t8132 support)            [installed via kmutil]
  → Linux (Asahi kernel tree + our t8132 DTs/drivers)
  → AuroraOS aarch64 rootfs → aurora-shell kiosk
```

- A separate APFS "Aurora" stub volume is created; `bputil` downgrades **only that volume** to Permissive Security. macOS's own volume and boot policy are never modified.
- m1n1 is installed as the volume's kernel object via `kmutil configure-boot`. m1n1 parses Apple's device tree (ADT) at runtime, so much of it is SoC-generic; the t8132 port work is CPU bring-up, AIC (interrupt controller) changes, and memory-map deltas.
- m1n1 chainloads Linux directly (or via U-Boot later, for a boot menu) with a flattened device tree we author (`t8132.dtsi` + `t8132-j61x.dts` board files).

**SPTM — the M4-specific wall (added 2026-07-10).** M4 introduces the Secure Page Table Monitor, running at GL2 (above the hypervisor). This **invalidates rung 2's method**: on M1/M2 the Asahi team reverse-engineered hardware by running macOS *under* m1n1's hypervisor and tracing every MMIO access — SPTM blocks that. Consequence: WS-B's timeline is **upstream-gated, not effort-gated**. Bare-metal M4 waits on the Asahi project settling a new RE methodology (static analysis of XNU / DriverKit binaries, AI-assisted disassembly to map undocumented registers — useful but not a substitute for hands-on RE); we rebase onto it rather than crack SPTM ourselves. This is the concrete reason WS-A (the VM track) is the value-delivering path and WS-B is explicitly research-grade with no committed date.

**16 KB pages — a non-issue for the rootfs (verified 2026-07-10).** Apple Silicon kernels use 16 KB pages. This is a *kernel* config (`CONFIG_ARM64_16K_PAGES`), applied in WS-B. It is **not** a toolchain/glibc concern: aarch64 userland is page-size-agnostic — GNU ld defaults `MAXPAGESIZE` to 64 KB, so segments align to 64 KB and the same binaries run on 4 KB, 16 KB, or 64 KB kernels; glibc reads the page size at runtime. Confirmed by `readelf` on a binary built by our toolchain (LOAD align = 0x10000). The WS-A rootfs built on a 4 KB host runs unchanged on a 16 KB Apple kernel.

### Two parallel workstreams

**WS-A — aarch64 AuroraOS rootfs (independent, ships early).**
Port the LFS build from x86_64 to aarch64: retarget `LFS_TGT`, adapt toolchain scripts (binutils/gcc/glibc pass1/2 are arch-clean in LFS; kernel + bootloader steps change), aarch64 kernel config, boot via UEFI (VM) — GRUB works on aarch64-EFI for the VM case. Deliverable: AuroraOS boots in a VM on the M4 (UTM / Apple Virtualization framework) with the full Aurora shell. This is required for bare metal anyway (it *is* the rootfs) and gives a working AuroraOS-on-M4 experience while WS-B climbs.

**WS-B — bare-metal bringup ladder (research track).**
Each rung has a concrete observable; we never claim progress without it:

| # | Milestone | Observable |
|---|-----------|-----------|
| 1 | Rig + stub volume + m1n1 boots far enough to run its USB proxy | proxyclient session from tethered host over USB-C |
| 2 | m1n1 hypervisor runs macOS under itself | HV trace log of MMIO accesses (the RE method) |
| 3 | Linux boots to initramfs console | serial/USB console shell; AIC + timers + SMP in our `t8132.dtsi` |
| 4 | Display via iBoot-provided framebuffer | pixels on the internal panel through `simpledrm` (no DCP RE needed) |
| 5 | NVMe (ANS) | `/dev/nvme0n1` visible; mount the Aurora rootfs volume |
| 6 | USB (dwc3 + tipd) | external keyboard/storage works |
| 7 | Internal keyboard/trackpad (SPI/dockchannel), Wi-Fi (brcmfmac) | usable laptop |
| 8 | Installer integration | adapted asahi-installer performs the APFS resize + volume setup end-to-end |

Rungs 3–7 are "drivers built from scratch" in practice: authoring device trees and porting/adapting Asahi's M1/M2 drivers to t8132 register/behavior deltas discovered via the rung-2 hypervisor traces.

### RE rig

- Tethered proxy host: this Windows PC via WSL2 + `usbipd-win` running the m1n1 proxyclient (Python). WSL2 must be installed first (admin + reboot).
- USB-C data cable between the PC and the MacBook Pro.
- All bringup experiments are m1n1 payloads loaded over the proxy — nothing is flashed; a failed experiment is cured by a reboot.

## Risks and fallbacks

- **m1n1 shows no life on t8132** after the first serious bringup sessions → WS-A keeps shipping (VM track); monitor upstream Asahi M3/M4 progress and rebase onto it the moment it lands. Nothing in WS-A or the DT/driver work is throwaway.
- **macOS updates** can move iBoot/ADT details; pin a known-good macOS version on the Aurora stub volume.
- **Recovery**: normal undo = delete Aurora APFS volume from recoveryOS. Worst case = DFU restore, which requires another Mac (accepted; see realities).
- **Work-device risk**: the M4 is assumed to be a personal/available machine; the Windows work PC is only a tethered host (no modifications beyond WSL2 install).

## Out of scope

- GPU driver (software rendering only)
- M4 Pro/Max-specific dies beyond the user's actual machine
- x86_64 boot-anywhere ISO improvements (separate, later project)
- Touch Bar-less niceties: Touch ID, webcam (ISP), speakers (they need the Asahi speaker-safety DSP work — explicitly deferred; headphones/USB audio first)

## Verification approach

- WS-A: golden path = QEMU aarch64 boot test of each build stage's output; final check = full shell boot in UTM on the M4.
- WS-B: every rung's observable above, captured (photo/serial log) before the rung is called done.
- Nothing merges to `main` without its milestone evidence noted in the commit.

## Prerequisites checklist (before any partitioning)

1. Full backup of the M4 (Time Machine or equivalent) — user-confirmed.
2. macOS updated to a chosen pinned version; note exact build number.
3. WSL2 + usbipd-win installed on the Windows PC (admin required).
4. USB-C data cable verified for data (not charge-only).
5. User acknowledges the no-second-Mac DFU limitation.
