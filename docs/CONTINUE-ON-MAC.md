# Continue AuroraOS aarch64 build on the M4 Mac

**Status as of 2026-07-10:** scripts **00 → 04 are validated end-to-end on real
aarch64** (in a QEMU-emulated Debian arm64 container on the Windows PC). The
cross-toolchain builds from source, passes the glibc sanity gate, and its
pass-2 gcc produces aarch64 binaries that link against
`/lib/ld-linux-aarch64.so.1` and run. Nine download/build bugs were found and
fixed live — all already on `main`. What remains untested is scripts **05–10**
(chroot, base system, kernel, bootloader, shell) — too slow to run emulated,
fast native on the M4.

Pull `main` and you have every fix. Nothing else to sync.

---

## 1. One-time setup on the Mac (build host)

Full detail in `docs/build-host-utm.md`. Short version:

1. Install UTM (`brew install --cask utm`).
2. Make a **Virtualize → Linux** VM from the **Debian 12 arm64** netinst ISO
   (https://cdimage.debian.org/debian-cd/current/arm64/iso-cd/).
   - 6+ CPU, 8+ GB RAM, 40 GB system disk.
   - Add a **second** VirtIO disk, 40 GB — this becomes `$LFS_DISK` and **will
     be wiped**. Inside the VM it is `/dev/vdb`.
3. Inside the VM, as root:

   ```bash
   apt update && apt install -y git build-essential m4 bison gawk texinfo gettext \
     python3 wget curl xz-utils file bc libssl-dev libelf-dev flex cpio \
     squashfs-tools xorriso mtools grub-efi-arm64-bin dosfstools parted \
     qemu-system-arm qemu-efi-aarch64 busybox-static sudo
   ln -sf bash /bin/sh
   git clone https://github.com/Aneek1/auroraos.git /root/auroraos
   cd /root/auroraos
   ```

   Then point `LFS_DISK` at the scratch disk before running script 01:

   ```bash
   sed -i 's|^export LFS_DISK=.*|export LFS_DISK=/dev/vdb|' config/build.conf
   ```

   (`AURORA_ARCH` auto-detects as `aarch64` inside the VM — no flags needed.)

## 2. Run the build, in order

```bash
bash scripts/00-check-host.sh          # ~1 min — expect "Host OK"
bash scripts/01-prepare-disk.sh        # wipes /dev/vdb, mounts $LFS
bash scripts/02-download-sources.sh    # ~20 min — now also pulls the ~0.8 GB Aura LLM model (GGUF)
bash scripts/03-toolchain-pass1.sh     # native ~1 h (was ~2 h emulated)
bash scripts/04-temp-tools.sh          # native ~1 h
sudo bash scripts/05-enter-chroot.sh   # drops you INTO the chroot
```

Then **inside the chroot**, run in order (05 prints this list too):

```bash
/aurora/scripts/06-base-system.sh      # the big one — native 3–6 h
/aurora/scripts/07-system-config.sh
/aurora/scripts/08-kernel.sh           # vanilla 6.13.4, Apple+VM-trimmed config
/aurora/scripts/09-bootloader.sh       # grub arm64-efi
/aurora/scripts/10-aurora-shell.sh     # wayland kiosk + firefox + shell + builds llama-server (1–3 h)
exit                                    # leave chroot
bash scripts/11-make-iso.sh            # optional live ISO
scripts/99-smoke-qemu.sh auroraos-1.0.iso   # boot it
```

Every script is idempotent via stamp files in
`$LFS/var/lib/aurora-build/` — a failure mid-way resumes where it stopped.

## 3. Watch-items entering script 06 (found during the smoke test)

These did **not** block 00–04 but are the first things to check when the base
system builds:

1. **libstdc++ went to `/usr/lib64` in the temp toolchain.** On pure aarch64
   there is no `lib64` convention (it's an x86 multilib holdover) and the
   dynamic loader searches `/usr/lib`. Harmless for the *temporary* pass-2
   compiler, but if the final gcc in script 06 (LFS ch. 8) installs C++ libs to
   `/usr/lib64`, C++ programs may fail to find `libstdc++.so.6` at runtime.
   If that happens: add `--libdir=/usr/lib` (or the LFS-standard gcc lib fix)
   to the ch.8 gcc recipe, or symlink `/usr/lib64 → /usr/lib` in the chroot.
2. **`05-enter-chroot.sh` line 7** chowns `$LFS/lib64` unconditionally, but it's
   guarded by `2>/dev/null || true`, so a missing lib64 on aarch64 is a no-op.
   Nothing to do; noted for completeness.

## 4. What was proven vs. what wasn't

| Scripts | Status | Where |
|---|---|---|
| 00–04 | ✅ run clean on aarch64, toolchain self-hosts | container, 2026-07-10 |
| 05–10 | ⏳ untested (too slow emulated) | run natively on the M4 |
| 11 | ⏳ untested | run natively on the M4 |

The riskiest arch-specific code (the whole toolchain — loader paths, target
triple, glibc, C++ headers) is the part that's proven. 05–10 are mostly
arch-neutral autotools packages plus the already-reviewed arch-aware bits
(kernel image path, grub target).

## 5. Aura on-device LLM (built 2026-07-10)

Aura (the assistant) now runs a real on-device model — **Llama-3.2-1B-Instruct**
(Q4, ~0.8 GB, bundled) via **llama.cpp**, behind aurorad's `/ask`. The model
proposes tool-calls into a fixed registry; deterministic Python validates and
executes them (whitelist-guarded, `power` never exposed, heuristic fallback if
the model is down). Design: `docs/superpowers/specs/2026-07-10-aura-local-llm-design.md`.

- **Pure-Python core is fully tested on any machine** (no model/ARM needed):
  `python3 -m pytest tests/ -v` → 31 tests. Run this on the Mac first to confirm
  the checkout is sound before the long build.
- Script 02 downloads the GGUF into `$LFS/opt/aura/models/` (GGUF-magic +
  size checked). Script 10 builds `llama-server` and installs two systemd units:
  `aura-llm.service` (runs the model as user `aurora` on 127.0.0.1:8080) and the
  updated `aurorad.service` (runs as **root** — needs backlight/systemctl — with
  `Wants=aura-llm.service`).
- On the running system, Aura gives smart replies only while `aura-llm.service`
  is up; otherwise `/ask` falls back to the deterministic heuristics and still
  works. UI actions ("open files", "tile for coding") are driven from the reply
  via `window.Aura.exec` in the shell.
- **Externals not yet fetch-verified from here:** llama.cpp tag `b4589` and the
  Hugging Face GGUF URL (`bartowski/Llama-3.2-1B-Instruct-GGUF`). If either 404s,
  bump the tag / find the current GGUF and update `config/extras.list` +
  `scripts/02-download-sources.sh`. The arm64 compile of llama.cpp was proven in
  the container (see project memory).

## 6. Bare metal (M4 SPTM) — later, not now

This VM build is **workstream A**. Bare-metal M4 (m1n1 / t8132) is workstream B,
gated on upstream Asahi solving the M4 **SPTM** problem: SPTM runs at GL2 and
blocks the hypervisor-tracing RE method Asahi used on M1/M2, so WS-B is
upstream-timeline-gated, not effort-gated. See
`docs/superpowers/specs/2026-07-10-m4-baremetal-design.md`. Note: aarch64
userland is **page-size-agnostic** (binaries link with 64 KB max-page-size, so
they run on a 16 KB-page Apple kernel unchanged) — the 16 KB page size is a
*kernel* config concern for WS-B, not a toolchain rebuild.
