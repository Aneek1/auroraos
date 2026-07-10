# AuroraOS — an LFS-based distro that boots into the "Windows 12 concept" shell

AuroraOS is built with **Linux From Scratch 12.3 (systemd)**. Instead of a
traditional desktop, it boots straight into a Wayland kiosk (`cage`) running a
web shell — the AuroraOS desktop UI (adapted from the Windows 12 concept) —
wired to real hardware through `aurorad`, a small localhost system API
(battery, brightness, power, files, app launch).

```
┌────────────────────────────────────────────────┐
│  shell/index.html  (desktop UI, runs in kiosk) │
│        │  fetch http://127.0.0.1:7212          │
│  aurorad.py  (system bridge, root service)     │
│        │  /sys, systemctl, subprocess          │
│  LFS 12.3 base system + Linux kernel           │
└────────────────────────────────────────────────┘
```

## Requirements

- A build host (or VM) with a distro that passes `00-check-host.sh` (Debian 12 / Ubuntu 24.04 work)
- **Architectures:** x86_64 and aarch64. The build host's arch is the target
  arch (`AURORA_ARCH` auto-detects). For aarch64 — including running AuroraOS
  on Apple Silicon in a VM — see `docs/build-host-utm.md`.
- A spare disk or virtual disk (≥ 30 GB) — **it will be wiped**
- 4+ cores, 8+ GB RAM, and patience: **6–12 hours of compilation** (~200 SBU total)
- Run everything as root inside the VM, from this repo's root

Quick QEMU build host:

```bash
qemu-img create -f qcow2 aurora-build.qcow2 40G
qemu-system-x86_64 -enable-kvm -m 8G -smp 4 \
  -drive file=debian12.qcow2 -drive file=aurora-build.qcow2 \
  -bios /usr/share/ovmf/OVMF.fd
```

## Build order

| # | Script | What it does | Time |
|---|--------|--------------|------|
| 0 | `scripts/00-check-host.sh` | Verify host tools per LFS ch. 2 | 1 min |
| 1 | `scripts/01-prepare-disk.sh` | Partition + mount `$LFS` (DESTRUCTIVE) | 1 min |
| 2 | `scripts/02-download-sources.sh` | Fetch LFS source list + extras, verify md5 | 10 min |
| 3 | `scripts/03-toolchain-pass1.sh` | Cross toolchain: binutils, gcc, glibc… (LFS ch. 5) | ~1 h |
| 4 | `scripts/04-temp-tools.sh` | Temporary tools (LFS ch. 6) | ~1 h |
| 5 | `scripts/05-enter-chroot.sh` | Mount virtual FS + chroot | — |
| 6 | `scripts/06-base-system.sh` | Full base system in chroot (LFS ch. 7–8) | 3–6 h |
| 7 | `scripts/07-system-config.sh` | Users, network, fstab, os-release, branding | 5 min |
| 8 | `scripts/08-kernel.sh` | Kernel with `config/kernel.fragment` | ~30 min |
| 9 | `scripts/09-bootloader.sh` | GRUB (UEFI) + AuroraOS entry | 5 min |
| 10 | `scripts/10-aurora-shell.sh` | Wayland/kiosk stack, Firefox, shell, aurorad, services | 1–3 h |
| 11 | `scripts/11-make-iso.sh` | Optional: squashfs live ISO | 20 min |

Scripts 3–4 run on the host; 6–10 run **inside the chroot** (script 5 prints the
exact command). Every script sources `config/build.conf` and is idempotent-ish:
finished packages are skipped via stamp files in `$LFS/var/lib/aurora-build/`.

## Honesty notes (read before building)

- These scripts **track the LFS 12.3 book**. LFS is precise; if a package fails,
  the book (https://www.linuxfromscratch.org/lfs/view/12.3-systemd/) is the
  canonical reference — fix per book, re-run, the stamp system resumes.
- `06-base-system.sh` implements the ~80 base packages as a recipe table:
  the fiddly ones (glibc, gcc, binutils, perl, python…) have explicit recipes;
  standard autotools packages go through a generic recipe.
- Firefox is installed as the official **prebuilt binary** (compiling it from
  source is a 20+ hour BLFS project). Its GTK3 runtime deps are built in
  script 10 from `config/extras.list` — this is the pragmatic deviation from
  "pure" LFS, and the longest part of script 10.
- The web shell is a concept UI. `aurorad` wires the real parts: clock, battery,
  brightness, power actions, file listing, app launch. The "Aura" assistant
  panel keeps canned/heuristic responses — bring your own model later (it was
  designed so an on-device LLM can be dropped behind `/ask`).

## After first boot

Login is automatic: `aurora-shell.service` starts `cage` + Firefox in kiosk
mode as the unprivileged `aurora` user; `aurorad.service` provides the system
bridge on 127.0.0.1:7212. TTY2 (Ctrl-Alt-F2) gives you a normal shell —
user `aneek`, password set during script 7.

## Roadmap: Apple Silicon bare metal

The aarch64 port is workstream A of the M4 bare-metal plan — see
`docs/superpowers/specs/2026-07-10-m4-baremetal-design.md`. Bare-metal M4
(m1n1/t8132 bringup) is workstream B and has its own milestone ladder.
