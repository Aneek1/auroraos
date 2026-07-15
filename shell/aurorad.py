#!/usr/bin/env python3
"""aurorad — AuroraOS system bridge.

Tiny localhost HTTP API the web shell talks to. Root service, binds
127.0.0.1 only (port from $AURORAD_PORT, default 7212). Endpoints:

  GET  /status            battery, brightness, hostname, load, uptime, net
  GET  /files?path=~/x    list a directory (confined to /home and /usr/share/aurora)
  POST /brightness        {"percent": 0-100}
  POST /power             {"action": "poweroff"|"reboot"|"lock"}
  POST /launch            {"app": "<whitelisted>"}
  POST /ask               {"q": "..."} -> aura_llm: on-device LLM + tool-calls
"""
import json, os, re, glob, subprocess, time, urllib.parse, urllib.request, shutil, threading
import aura_llm
from http.server import HTTPServer, BaseHTTPRequestHandler

# sbin tools (wipefs, sfdisk, mkfs.*) must resolve regardless of who starts us
# — the labwc session PATH has no sbin, which broke installs with FileNotFoundError.
os.environ["PATH"] = "/usr/sbin:/usr/bin:/sbin:/bin:" + os.environ.get("PATH", "")

PORT = int(os.environ.get("AURORAD_PORT", "7212"))
LAUNCH_WHITELIST = {"terminal": ["foot"], "editor": ["foot", "vi"]}
START = time.time()
HOME = "/var/lib/aurora" if os.path.isdir("/var/lib/aurora") else os.path.expanduser("~")
APP_DIRS = ["/usr/share/applications",
            HOME + "/.local/share/applications",
            HOME + "/.nix-profile/share/applications",
            "/var/lib/flatpak/exports/share/applications"]

def _parse_desktop(path):
    """Return {name, exec, icon} for a real, visible Application .desktop, else None."""
    name = exec_ = icon = None; is_app = True; nodisplay = False
    try:
        with open(path, encoding="utf-8", errors="ignore") as f:
            section = None
            for line in f:
                line = line.rstrip("\n")
                if line.startswith("["): section = line; continue
                if section != "[Desktop Entry]": continue
                if line.startswith("Name=") and not name: name = line[5:]
                elif line.startswith("Exec="): exec_ = line[5:]
                elif line.startswith("Icon="): icon = line[5:]
                elif line.startswith("Type=") and line[5:] != "Application": is_app = False
                elif line.startswith("NoDisplay=") and line[10:].lower() == "true": nodisplay = True
    except OSError:
        return None
    if not (name and exec_ and is_app) or nodisplay:
        return None
    exec_ = re.sub(r"%[UuFfickvmND]", "", exec_).strip()  # drop freedesktop field codes
    return {"name": name, "exec": exec_, "icon": icon or ""}

def discover_apps():
    """Scan standard app dirs + Nix profile + ~/Apps AppImages -> {id: {name,exec,icon}}."""
    apps = {}
    for d in APP_DIRS:
        if not os.path.isdir(d): continue
        for fn in os.listdir(d):
            if fn.endswith(".desktop"):
                info = _parse_desktop(os.path.join(d, fn))
                if info: apps.setdefault(fn[:-8], info)
    appdir = HOME + "/Apps"
    if os.path.isdir(appdir):
        for fn in os.listdir(appdir):
            if fn.lower().endswith(".appimage"):
                apps.setdefault("appimage:" + fn,
                                {"name": fn[:-9], "exec": os.path.join(appdir, fn), "icon": ""})
    return apps

def read1(path):
    try:
        with open(path) as f: return f.read().strip()
    except OSError: return None

def battery():
    for supply in glob.glob("/sys/class/power_supply/*"):
        if read1(supply + "/type") == "Battery":
            cap = read1(supply + "/capacity")
            return {"percent": int(cap) if cap else None,
                    "status": read1(supply + "/status")}
    return {"percent": None, "status": "AC"}

def backlight_dev():
    devs = glob.glob("/sys/class/backlight/*")
    return devs[0] if devs else None

def brightness_get():
    d = backlight_dev()
    if not d: return None
    cur, mx = read1(d + "/brightness"), read1(d + "/max_brightness")
    return round(int(cur) / int(mx) * 100) if cur and mx else None

def brightness_set(pct):
    d = backlight_dev()
    if not d: return False
    mx = int(read1(d + "/max_brightness") or 100)
    val = max(1, min(mx, round(mx * pct / 100)))
    with open(d + "/brightness", "w") as f: f.write(str(val))
    return True

def net_up():
    for iface in glob.glob("/sys/class/net/*"):
        if os.path.basename(iface) != "lo" and read1(iface + "/operstate") == "up":
            return os.path.basename(iface)
    return None

def safe_path(p):
    p = os.path.realpath(os.path.expanduser(p or "~"))
    if p.startswith(("/home", "/usr/share/aurora", "/tmp")): return p
    return "/home"

# ---------------- Aurora Store: catalog + AppImage install ----------------
# Apps are installed by downloading their AppImage, extracting it (no FUSE
# needed via --appimage-extract), and writing a .desktop into the user's app
# dir so it appears in the launcher/dock like any other app.
APPS_HOME   = HOME + "/Applications"
DESKTOP_DIR = HOME + "/.local/share/applications"

def _catalog_path():
    here = os.path.dirname(os.path.abspath(__file__))
    for c in ("/usr/share/aurora/store/catalog",
              "/opt/aura/store/catalog",
              os.path.join(here, "..", "store", "catalog"),
              "/aurora/store/catalog"):
        if os.path.exists(c):
            return c
    return "/usr/share/aurora/store/catalog"

def load_catalog():
    """Catalog is pipe-delimited so the C shell can read the same file:
       id|name|category|icon|description|github-repo-or-direct-url"""
    apps = []
    try:
        with open(_catalog_path(), encoding="utf-8") as f:
            for line in f:
                line = line.rstrip("\n")
                if not line or line.startswith("#"):
                    continue
                p = line.split("|")
                if len(p) < 6:
                    continue
                app = {"id": p[0].strip(), "name": p[1].strip(), "category": p[2].strip(),
                       "icon": p[3].strip(), "description": p[4].strip()}
                src = p[5].strip()
                app["url" if src.startswith("http") else "github"] = src
                apps.append(app)
    except OSError:
        pass
    return apps

def _installed_store_ids():
    if not os.path.isdir(APPS_HOME):
        return set()
    return {d for d in os.listdir(APPS_HOME)
            if os.path.isdir(os.path.join(APPS_HOME, d))}

def store_catalog():
    inst = _installed_store_ids()
    return {"apps": [{**a, "installed": a.get("id") in inst} for a in load_catalog()]}

def _resolve_url(app):
    """A catalog entry may pin a direct 'url' or name a 'github' repo whose latest
    release's x86_64 AppImage we resolve at install time (survives version bumps)."""
    if app.get("url"):
        return app["url"]
    gh = app.get("github")
    if not gh:
        return None
    try:
        api = "https://api.github.com/repos/%s/releases/latest" % gh
        req = urllib.request.Request(api, headers={"User-Agent": "AuroraStore/1.0"})
        assets = json.loads(urllib.request.urlopen(req, timeout=45).read()).get("assets", [])
        for a in assets:
            if "x86_64" in a.get("name", "") and a["name"].endswith(".AppImage"):
                return a["browser_download_url"]
        for a in assets:
            if a.get("name", "").endswith(".AppImage"):
                return a["browser_download_url"]
    except Exception:
        return None
    return None

def _ar_members(path):
    """Yield (name, bytes) for each member of a Unix 'ar' archive (the .deb container)."""
    with open(path, "rb") as f:
        if f.read(8) != b"!<arch>\n":
            return
        while True:
            hdr = f.read(60)
            if len(hdr) < 60:
                break
            name = hdr[0:16].decode("ascii", "replace").strip().rstrip("/")
            try:
                size = int(hdr[48:58].decode("ascii").strip() or "0")
            except ValueError:
                break
            data = f.read(size)
            if size & 1:
                f.read(1)  # members are 2-byte aligned
            yield name, data


def _dest_desktop(dest, app_id, app):
    """Build a launcher for an app tree unpacked under dest (from a .deb's data.tar).
    Reads the app's own bundled .desktop, remaps its absolute Exec/Icon into dest, and
    appends --no-sandbox for Electron (the unpacked chrome-sandbox isn't setuid root)."""
    import shlex
    apps_dir = os.path.join(dest, "usr", "share", "applications")
    exec_line = icon = name = None
    if os.path.isdir(apps_dir):
        cands = sorted(f for f in os.listdir(apps_dir)
                       if f.endswith(".desktop") and "url-handler" not in f)
        for fn in cands:
            try:
                for l in open(os.path.join(apps_dir, fn), encoding="utf-8", errors="replace"):
                    if l.startswith("Exec=") and not exec_line: exec_line = l[5:].strip()
                    elif l.startswith("Icon=") and not icon:     icon = l[5:].strip()
                    elif l.startswith("Name=") and not name:     name = l[5:].strip()
            except OSError:
                continue
            if exec_line:
                break
    # resolve the executable to a real path inside dest
    binpath = None
    if exec_line:
        toks = [t for t in shlex.split(exec_line) if not t.startswith("%")]
        if toks:
            b = toks[0]
            cand = os.path.join(dest, b.lstrip("/")) if b.startswith("/") \
                   else os.path.join(dest, "usr", "bin", b)
            if os.path.islink(cand):  # deb symlinks are absolute -> remap into dest
                tgt = os.readlink(cand)
                cand = os.path.join(dest, tgt.lstrip("/")) if tgt.startswith("/") \
                       else os.path.realpath(os.path.join(os.path.dirname(cand), tgt))
            if os.path.exists(cand):
                binpath = cand
    if not binpath:  # fallback: first executable under usr/bin
        ub = os.path.join(dest, "usr", "bin")
        if os.path.isdir(ub):
            for f in sorted(os.listdir(ub)):
                p = os.path.join(ub, f)
                if os.path.isfile(p) and os.access(p, os.X_OK):
                    binpath = p; break
    if not binpath:
        return None, None
    # electron apps need --no-sandbox when unpacked (no setuid chrome-sandbox)
    is_electron = os.path.exists(os.path.join(os.path.dirname(binpath), "chrome-sandbox")) \
                  or os.path.exists(os.path.join(os.path.dirname(binpath), "chrome_crashpad_handler"))
    cmd = shlex.quote(binpath) + (" --no-sandbox" if is_electron else "")
    # resolve an icon file inside dest, else keep the themed name
    icon_path = icon or app.get("icon", "")
    if icon and not icon.startswith("/"):
        for root in ("usr/share/pixmaps", "usr/share/icons"):
            base = os.path.join(dest, root)
            for dp, _dn, fns in os.walk(base) if os.path.isdir(base) else []:
                for fn in fns:
                    if fn.startswith(icon + ".") and fn.rsplit(".", 1)[-1] in ("png", "svg", "xpm"):
                        icon_path = os.path.join(dp, fn); break
                if icon_path.startswith("/"): break
            if icon_path.startswith("/"): break
    return cmd, (icon_path if icon_path.startswith("/") else "application-x-executable")


def _install_deb(debpath, app_id, app):
    """Install a Debian package without dpkg: unpack its data.tar into ~/Applications/<id>
    and register a launcher pointing at the app inside that tree."""
    import io, tarfile
    blob = comp = None
    for name, data in _ar_members(debpath):
        if name.startswith("data.tar"):
            blob, comp = data, name.rsplit(".", 1)[-1]
            break
    if blob is None:
        return {"ok": False, "error": "not a valid .deb (no data.tar)"}
    # Stream-decompress straight into tarfile (mode "r|<c>") rather than materialising
    # the whole ~550MB decompressed archive in RAM — the live home is tmpfs, so peak
    # memory matters. zstd (rare) is piped through the zstd tool first.
    mode = {"xz": "r|xz", "lzma": "r|xz", "gz": "r|gz", "bz2": "r|bz2", "tar": "r|"}.get(comp)
    src = blob
    if mode is None:
        if comp == "zst":
            z = subprocess.run(["zstd", "-d", "-c"], input=blob,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            if z.returncode != 0:
                return {"ok": False, "error": "this .deb uses zstd; zstd tool unavailable"}
            src, mode = z.stdout, "r|"
        else:
            return {"ok": False, "error": "unsupported .deb compression: %s" % comp}
    dest = os.path.join(APPS_HOME, app_id)
    if os.path.islink(dest) or os.path.isfile(dest):
        os.remove(dest)
    else:
        shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest, exist_ok=True)
    try:
        with tarfile.open(fileobj=io.BytesIO(src), mode=mode) as t:
            try:
                t.extractall(dest, filter="data")   # py3.12+: strips setuid/absolute paths
            except TypeError:
                t.extractall(dest)
    except Exception as e:
        return {"ok": False, "error": "could not unpack .deb: %s" % e}
    cmd, icon = _dest_desktop(dest, app_id, app)
    if not cmd:
        return {"ok": False, "error": "installed, but no launchable program was found in the package"}
    os.makedirs(DESKTOP_DIR, exist_ok=True)
    with open(os.path.join(DESKTOP_DIR, "aurora-store-%s.desktop" % app_id), "w",
              encoding="utf-8") as f:
        f.write("[Desktop Entry]\nType=Application\n"
                "Name=%s\nComment=%s\nExec=%s\nIcon=%s\nCategories=%s;\nTerminal=false\n"
                % (app.get("name", app_id), app.get("description", ""), cmd,
                   icon, app.get("category", "Utility")))
    return {"ok": True, "installed": app.get("name", app_id)}


def store_install(app_id):
    app = {a.get("id"): a for a in load_catalog()}.get(app_id)
    if not app:
        return {"ok": False, "error": "unknown app"}
    url = _resolve_url(app)
    if not url:
        return {"ok": False, "error": "couldn't find a download for this app"}
    os.makedirs(APPS_HOME, exist_ok=True)
    os.makedirs(DESKTOP_DIR, exist_ok=True)
    tmp = "/tmp/%s.AppImage" % app_id
    work = "/tmp/extract-%s" % app_id
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "AuroraStore/1.0"})
        with urllib.request.urlopen(req, timeout=600) as r, open(tmp, "wb") as f:
            shutil.copyfileobj(r, f)
        os.chmod(tmp, 0o755)
        # A .deb is a Unix 'ar' archive; detect by magic and take the dpkg-free path.
        with open(tmp, "rb") as mf:
            magic = mf.read(8)
        if magic == b"!<arch>\n":
            return _install_deb(tmp, app_id, app)
        shutil.rmtree(work, ignore_errors=True)
        os.makedirs(work, exist_ok=True)
        ex = subprocess.run([tmp, "--appimage-extract"], cwd=work,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # Some AppImages (e.g. pkgforge "anylinux" builds) extract to AppDir/ and
        # leave squashfs-root as a compat *symlink* to it. Resolve to the real
        # directory so we move the tree, not a link that dangles once relocated.
        src = os.path.realpath(os.path.join(work, "squashfs-root"))
        if ex.returncode != 0 or not os.path.isdir(src):
            return {"ok": False, "error": "could not unpack the app"}
        dest = os.path.join(APPS_HOME, app_id)
        # Clear any prior install robustly: dest may be a real dir, a broken/stale
        # symlink, or a plain file. rmtree(ignore_errors) silently no-ops on a
        # symlink, which would make the move land *inside* the old target — so
        # handle links/files explicitly before falling back to rmtree.
        if os.path.islink(dest) or os.path.isfile(dest):
            os.remove(dest)
        else:
            shutil.rmtree(dest, ignore_errors=True)
        shutil.move(src, dest)
        if not os.path.exists(os.path.join(dest, "AppRun")):
            return {"ok": False, "error": "app unpacked but no AppRun launcher was found"}
        # Do NOT force the Wayland backend: many AppImages bundle a GTK that crashes
        # on Wayland (their linuxdeploy hook sets GDK_BACKEND=x11 for exactly that
        # reason). We ship Xwayland, so leaving them on X11 is both stable and correct.
        icon = os.path.join(dest, ".DirIcon")
        with open(os.path.join(DESKTOP_DIR, "aurora-store-%s.desktop" % app_id),
                  "w", encoding="utf-8") as f:
            f.write("[Desktop Entry]\nType=Application\n"
                    "Name=%s\nComment=%s\nExec=%s\nIcon=%s\nCategories=%s;\nTerminal=false\n"
                    % (app.get("name", app_id), app.get("description", ""),
                       os.path.join(dest, "AppRun"),
                       icon if os.path.exists(icon) else "application-x-executable",
                       app.get("category", "Utility")))
        return {"ok": True, "installed": app.get("name", app_id)}
    except Exception as e:
        return {"ok": False, "error": "install failed: %s" % e}
    finally:
        try: os.remove(tmp)
        except OSError: pass
        shutil.rmtree(work, ignore_errors=True)

def store_remove(app_id):
    shutil.rmtree(os.path.join(APPS_HOME, app_id), ignore_errors=True)
    d = os.path.join(DESKTOP_DIR, "aurora-store-%s.desktop" % app_id)
    try: os.remove(d)
    except OSError: pass
    return {"ok": True}

# ----- persistent storage -----------------------------------------------
# The live root is overlay(lowerdir=squashfs, upperdir=tmpfs), so every change
# lives in RAM and is lost on reboot. If a disk is labelled AURORA_DATA the
# initramfs uses it as the overlay upperdir instead of tmpfs, making the whole
# system delta (installed apps, pins, settings, home) persist. persist_setup()
# formats a blank attached disk with that label; the user reboots to activate.
PERSIST_LABEL = "AURORA_DATA"

def _lsblk_disks():
    """Whole disks as dicts {name, fstype, child(bool), size, ro(bool)}."""
    try:
        out = subprocess.check_output(
            ["lsblk", "-Ppno", "NAME,TYPE,FSTYPE,SIZE,RO"], text=True)
    except Exception:
        return []
    rows = [dict(re.findall(r'(\w+)="([^"]*)"', ln)) for ln in out.splitlines() if ln.strip()]
    disks = [r for r in rows if r.get("TYPE") == "disk"]
    parts = [r for r in rows if r.get("TYPE") == "part"]
    res = []
    for d in disks:
        name = d.get("NAME", "")
        res.append({"name": name, "fstype": d.get("FSTYPE", ""),
                    "child": any(p.get("NAME", "").startswith(name) for p in parts),
                    "size": d.get("SIZE", ""), "ro": d.get("RO") == "1"})
    return res

def _blank_disk():
    """A whole, writable disk with no filesystem and no partitions, or None.
    Never returns a disk that already holds data — safe to format."""
    for d in _lsblk_disks():
        if not d["ro"] and d["fstype"] == "" and not d["child"]:
            return d["name"]
    return None

def persist_status():
    return {"active": os.path.exists("/dev/disk/by-label/" + PERSIST_LABEL),
            "candidate": _blank_disk()}

def persist_setup():
    st = persist_status()
    if st["active"]:
        return {"ok": True, "already": True,
                "message": "Persistent storage is already active."}
    dev = st["candidate"]
    if not dev:
        return {"error": "No blank disk found. Add a second, empty virtual "
                         "hard disk to the VM (VirtualBox: Settings > Storage > "
                         "add a new VDI), then try again."}
    try:
        subprocess.check_output(
            ["mkfs.ext4", "-F", "-q", "-L", PERSIST_LABEL, dev],
            stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError as e:
        return {"error": "Format failed: " + (e.output or b"").decode("utf-8", "replace")[:200]}
    except Exception as e:
        return {"error": "Format failed: %s" % e}
    return {"ok": True, "device": dev,
            "message": "Persistent storage ready on %s. Restart now — from then "
                       "on your apps and settings are saved across reboots." % dev}

# ----- install to disk --------------------------------------------------
# The ISO is a live image (root = overlay of squashfs + tmpfs). This installs
# a permanent copy onto a real disk: GPT (ESP + ext4 root), copy the pristine
# squashfs system, install GRUB (UEFI, removable path), write a UUID fstab.
# After this the machine boots from disk with a normal read-write root — the
# ISO can be ejected. Runs in a background thread; the shell polls INSTALL.
INSTALL = {"running": False, "stage": "idle", "pct": 0, "done": False, "error": ""}
INSTALL_LOCK = threading.Lock()

def _ist(stage, pct):
    with INSTALL_LOCK:
        INSTALL["stage"] = stage
        INSTALL["pct"] = pct

def list_target_disks():
    """Whole, writable disks usable as an install target (dev, size, model)."""
    try:
        out = subprocess.check_output(
            ["lsblk", "-Ppdno", "NAME,TYPE,SIZE,MODEL,RO"], text=True)
    except Exception:
        return []
    disks = []
    for ln in out.splitlines():
        d = dict(re.findall(r'(\w+)="([^"]*)"', ln))
        if d.get("TYPE") == "disk" and d.get("RO") != "1":
            disks.append({"dev": d.get("NAME", ""), "size": d.get("SIZE", ""),
                          "model": (d.get("MODEL") or "Disk").strip() or "Disk"})
    return disks

def _part_names(disk):
    # nvme0n1 -> p1/p2 ; sda -> 1/2
    base = os.path.basename(disk)
    if re.search(r'\d$', base):   # nvme/mmc end in a digit
        return disk + "p1", disk + "p2"
    return disk + "1", disk + "2"

def _uuid(dev):
    return subprocess.check_output(
        ["blkid", "-s", "UUID", "-o", "value", dev], text=True).strip()

def _partuuid(dev):
    # For root= on the kernel command line: a raw kernel (no initramfs) can
    # only resolve PARTUUID=, not filesystem UUID= (that needs userspace) —
    # root=UUID= panics with "VFS: unable to mount root fs".
    return subprocess.check_output(
        ["blkid", "-s", "PARTUUID", "-o", "value", dev], text=True).strip()

def _run(cmd, **kw):
    subprocess.run(cmd, check=True, stdout=subprocess.PIPE,
                   stderr=subprocess.STDOUT, **kw)

def _do_install(disk):
    T = "/mnt/target"
    src = "/mnt/ro" if os.path.ismount("/mnt/ro") else "/"
    esp, root = _part_names(disk)
    try:
        _ist("Partitioning disk", 4)
        _run(["wipefs", "-a", disk])
        # GPT: 512M EFI System partition (type U, bootable) + rest Linux (type L)
        subprocess.run(["sfdisk", disk], check=True, text=True,
                       input="label: gpt\n,512M,U,*\n,,L\n",
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        subprocess.run(["udevadm", "settle"], check=False)
        for _ in range(20):
            if os.path.exists(esp) and os.path.exists(root):
                break
            time.sleep(0.5)

        _ist("Formatting partitions", 12)
        _run(["mkfs.vfat", "-F32", "-n", "EFI", esp])
        _run(["mkfs.ext4", "-F", "-q", "-L", "aurora", root])

        _ist("Mounting target", 18)
        os.makedirs(T, exist_ok=True)
        _run(["mount", root, T])
        os.makedirs(T + "/boot/efi", exist_ok=True)
        _run(["mount", esp, T + "/boot/efi"])

        # copy the pristine system, top-level entry by entry for progress
        skip = {"proc", "sys", "dev", "run", "tmp", "mnt", "media", "lost+found"}
        entries = [e for e in sorted(os.listdir(src)) if e not in skip]
        for i, e in enumerate(entries):
            _ist("Copying system (%s)" % e, 20 + int(60 * i / max(1, len(entries))))
            _run(["cp", "-a", os.path.join(src, e), T + "/"])
        for e in ("proc", "sys", "dev", "run", "tmp", "mnt", "media"):
            os.makedirs(os.path.join(T, e), exist_ok=True)

        _ist("Installing bootloader", 84)
        for m, opt in (("dev", ["--bind"]), ("dev/pts", ["--bind"]),
                       ("proc", ["-t", "proc"]), ("sys", ["-t", "sysfs"])):
            tgt = os.path.join(T, m)
            os.makedirs(tgt, exist_ok=True)
            if "--bind" in opt:
                _run(["mount", "--bind", "/" + m, tgt])
            else:
                _run(["mount"] + opt + ["none", tgt])
        _run(["chroot", T, "grub-install", "--target=x86_64-efi",
              "--efi-directory=/boot/efi", "--bootloader-id=AuroraOS",
              "--removable", "--recheck"])

        _ist("Writing boot config", 92)
        ruuid = _uuid(root)
        rpart = _partuuid(root)
        with open(T + "/boot/grub/grub.cfg", "w") as f:
            f.write(
                "set default=0\nset timeout=2\n"
                "insmod part_gpt\ninsmod ext2\ninsmod all_video\n"
                "set gfxpayload=keep\n\n"
                'menuentry "AuroraOS 1.0 — daybreak" {\n'
                "  linux /boot/vmlinuz-aurora root=PARTUUID=%s rw quiet loglevel=3 "
                "vt.global_cursor_default=0 video=1920x1080\n}\n\n"
                'menuentry "AuroraOS (verbose / rescue)" {\n'
                "  linux /boot/vmlinuz-aurora root=PARTUUID=%s rw "
                "systemd.unit=multi-user.target\n}\n" % (rpart, rpart))

        _ist("Writing fstab", 96)
        euuid = _uuid(esp)
        with open(T + "/etc/fstab", "w") as f:
            f.write("# AuroraOS — written by the installer\n"
                    "UUID=%s  /          ext4  defaults,noatime  0 1\n"
                    "UUID=%s  /boot/efi  vfat  umask=0077        0 2\n"
                    % (ruuid, euuid))

        for m in ("dev/pts", "dev", "proc", "sys", "boot/efi", ""):
            subprocess.run(["umount", "-lf", os.path.join(T, m)], check=False)
        _ist("Done", 100)
        with INSTALL_LOCK:
            INSTALL["done"] = True
    except subprocess.CalledProcessError as e:
        out = (e.output or b"")
        out = out.decode("utf-8", "replace") if isinstance(out, bytes) else str(out)
        with INSTALL_LOCK:
            INSTALL["error"] = ("%s failed: %s" % (e.cmd[0] if e.cmd else "step",
                                                   out[-300:])) or "install failed"
    except Exception as e:
        with INSTALL_LOCK:
            INSTALL["error"] = str(e)
    finally:
        for m in ("dev/pts", "dev", "proc", "sys", "boot/efi", ""):
            subprocess.run(["umount", "-lf", os.path.join(T, m)], check=False)
        with INSTALL_LOCK:
            INSTALL["running"] = False

def install_start(disk):
    valid = {d["dev"] for d in list_target_disks()}
    if disk not in valid:
        return {"error": "Unknown or unusable disk: %r" % disk}
    with INSTALL_LOCK:
        if INSTALL["running"]:
            return {"error": "An installation is already in progress."}
        INSTALL.update(running=True, stage="Starting", pct=0, done=False, error="")
    threading.Thread(target=_do_install, args=(disk,), daemon=True).start()
    return {"ok": True}

def install_progress():
    with INSTALL_LOCK:
        return dict(INSTALL)

# ----- network drives (SMB via kernel cifs) ------------------------------
# Mounts //host/share at /media/<name> so the Files app sees it as a normal
# folder. No cifs-utils needed: we resolve the hostname here and pass ip=
# so the raw mount(2) path works. uid/gid map the files to the aurora user.
def _safe_mount_name(host, share):
    return re.sub(r"[^A-Za-z0-9._-]", "_", "%s_%s" % (host, share))[:64]

def list_shares():
    out = []
    try:
        with open("/proc/mounts") as f:
            for ln in f:
                parts = ln.split()
                if len(parts) >= 3 and parts[2] in ("cifs", "smb3", "nfs", "nfs4"):
                    out.append({"source": parts[0], "target": parts[1],
                                "fstype": parts[2]})
    except OSError:
        pass
    return out

def mount_share(host, share, user, pw):
    import socket as _s
    host = (host or "").strip().lstrip("\\/")
    share = (share or "").strip().strip("\\/")
    if not host or not share:
        return {"error": "Server and share name are both required."}
    try:
        ip = _s.gethostbyname(host)
    except OSError:
        return {"error": "Cannot resolve server '%s' — check the name and "
                         "that the network is up." % host}
    tgt = "/media/" + _safe_mount_name(host, share)
    if any(m["target"] == tgt for m in list_shares()):
        return {"ok": True, "already": True, "target": tgt,
                "message": "Already connected at %s" % tgt}
    os.makedirs(tgt, exist_ok=True)
    opts = "ip=%s,uid=999,gid=998,iocharset=utf8" % ip
    opts += ",username=%s" % (user or "guest")
    opts += (",password=%s" % pw) if pw else ",guest"
    r = subprocess.run(["mount", "-t", "cifs", "//%s/%s" % (host, share), tgt,
                        "-o", opts], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        try: os.rmdir(tgt)
        except OSError: pass
        tail = (r.stdout or "").strip()[-220:]
        return {"error": "Mount failed: %s" % (tail or "rc=%d" % r.returncode)}
    return {"ok": True, "target": tgt,
            "message": "Connected — open %s in Files." % tgt}

def unmount_share(target):
    if not target or not target.startswith("/media/"):
        return {"error": "bad target"}
    r = subprocess.run(["umount", target], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        return {"error": (r.stdout or "").strip()[-200:] or "umount failed"}
    try: os.rmdir(target)
    except OSError: pass
    return {"ok": True}

# ----- Aura model download (post-install) -------------------------------
# The ISO ships without the LLM model to stay small. Once installed and online,
# Aura downloads a ~0.8 GB model on demand; aura-llm-launch then serves it.
AURA_MODEL_DIR = "/opt/aura/models"
AURA_MODEL_URL = os.environ.get("AURA_MODEL_URL",
    "https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF/"
    "resolve/main/Llama-3.2-1B-Instruct-Q4_K_M.gguf")
AURA = {"running": False, "pct": 0, "done": False, "error": ""}
AURA_LOCK = threading.Lock()

def aura_model_present():
    try:
        return any(f.endswith(".gguf") for f in os.listdir(AURA_MODEL_DIR))
    except OSError:
        return False

def _aura_download():
    dest = os.path.join(AURA_MODEL_DIR, "Llama-3.2-1B-Instruct-Q4_K_M.gguf")
    tmp = dest + ".part"
    try:
        os.makedirs(AURA_MODEL_DIR, exist_ok=True)
        req = urllib.request.Request(AURA_MODEL_URL, headers={"User-Agent": "AuroraOS"})
        with urllib.request.urlopen(req, timeout=60) as r:
            total = int(r.headers.get("Content-Length", 0))
            got = 0
            with open(tmp, "wb") as f:
                while True:
                    chunk = r.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk); got += len(chunk)
                    if total:
                        with AURA_LOCK:
                            AURA["pct"] = int(got * 100 / total)
        os.replace(tmp, dest)
        subprocess.Popen(["/bin/sh", "/usr/lib/aurora/aura-llm-launch"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with AURA_LOCK:
            AURA.update(done=True, pct=100)
    except Exception as e:
        try: os.remove(tmp)
        except OSError: pass
        with AURA_LOCK:
            AURA["error"] = str(e)
    finally:
        with AURA_LOCK:
            AURA["running"] = False

def aura_setup_start():
    if aura_model_present():
        return {"ok": True, "already": True, "message": "Aura is already set up."}
    with AURA_LOCK:
        if AURA["running"]:
            return {"error": "Download already in progress."}
        AURA.update(running=True, pct=0, done=False, error="")
    threading.Thread(target=_aura_download, daemon=True).start()
    return {"ok": True}

def aura_status():
    with AURA_LOCK:
        s = dict(AURA)
    s["installed"] = aura_model_present()
    return s

class H(BaseHTTPRequestHandler):
    def _send(self, obj, code=200):
        # ensure_ascii=False so unicode (em-dashes, accents, emoji) goes out as
        # UTF-8 rather than \uXXXX escapes the shell's tiny JSON reader can't decode.
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self): self._send({})

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)
        if url.path == "/status":
            self._send({"battery": battery(), "brightness": brightness_get(),
                        "hostname": read1("/etc/hostname") or "aurora",
                        "load": os.getloadavg()[0], "uptime": int(time.time() - START),
                        "net": net_up(),
                        "os": "AuroraOS 1.0 (daybreak)"})
        elif url.path == "/files":
            path = safe_path(q.get("path", ["~"])[0])
            try:
                items = [{"name": e.name, "dir": e.is_dir()}
                         for e in sorted(os.scandir(path), key=lambda e: (not e.is_dir(), e.name))
                         if not e.name.startswith(".")][:200]
                self._send({"path": path, "items": items})
            except OSError as e:
                self._send({"error": str(e)}, 404)
        elif url.path == "/apps":
            apps = discover_apps()
            self._send({"apps": [{"id": k, "name": v["name"], "icon": v["icon"]}
                                 for k, v in sorted(apps.items(), key=lambda kv: kv[1]["name"].lower())]})
        elif url.path == "/store/catalog":
            self._send(store_catalog())
        elif url.path == "/system/persist-status":
            self._send(persist_status())
        elif url.path == "/system/disks":
            disks = list_target_disks()
            self._send({"disks": disks,
                        "list": "\n".join("%s|%s|%s" % (d["dev"], d["size"], d["model"])
                                          for d in disks)})
        elif url.path == "/system/install-progress":
            self._send(install_progress())
        elif url.path == "/system/aura-status":
            self._send(aura_status())
        elif url.path == "/system/shares":
            sh = list_shares()
            self._send({"shares": sh,
                        "list": "\n".join("%s|%s|%s" % (m["target"], m["source"], m["fstype"])
                                          for m in sh)})
        else:
            self._send({"error": "not found"}, 404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        try: data = json.loads(self.rfile.read(n) or b"{}")
        except json.JSONDecodeError: return self._send({"error": "bad json"}, 400)
        if self.path == "/store/install":
            self._send(store_install(data.get("id", "")))
        elif self.path == "/system/persist-setup":
            self._send(persist_setup())
        elif self.path == "/system/install":
            self._send(install_start(data.get("disk", "")))
        elif self.path == "/system/aura-setup":
            self._send(aura_setup_start())
        elif self.path == "/system/mount-share":
            self._send(mount_share(data.get("host", ""), data.get("share", ""),
                                   data.get("user", ""), data.get("pass", "")))
        elif self.path == "/system/unmount-share":
            self._send(unmount_share(data.get("target", "")))
        elif self.path == "/system/power":
            # power must run as root: without polkit, systemd denies reboot/
            # poweroff from the session user (buttons silently did nothing)
            act = data.get("action")
            if act in ("poweroff", "reboot"):
                self._send({"ok": True})
                subprocess.Popen(["systemctl", act])
            else:
                self._send({"error": "bad action"}, 400)
        elif self.path == "/store/remove":
            self._send(store_remove(data.get("id", "")))
        elif self.path == "/brightness":
            ok = brightness_set(int(data.get("percent", 50)))
            self._send({"ok": ok})
        elif self.path == "/power":
            act = data.get("action")
            if act in ("poweroff", "reboot"):
                self._send({"ok": True})
                subprocess.Popen(["systemctl", act])
            elif act == "lock":
                self._send({"ok": True})  # shell handles its own lock UI
            else:
                self._send({"error": "bad action"}, 400)
        elif self.path == "/launch":
            aid = data.get("id") or data.get("app", "")
            env = {**os.environ, "WAYLAND_DISPLAY": "wayland-0", "MOZ_ENABLE_WAYLAND": "1"}
            info = discover_apps().get(aid)
            if info:  # spawn the discovered app's Exec (came from a real .desktop, not user text)
                try:
                    subprocess.Popen(info["exec"], shell=True, env=env)
                    self._send({"ok": True, "launched": info["name"]})
                except OSError as e:
                    self._send({"error": str(e)}, 500)
            elif aid in LAUNCH_WHITELIST:  # legacy aliases (terminal/editor)
                subprocess.Popen(LAUNCH_WHITELIST[aid], env=env)
                self._send({"ok": True})
            else:
                self._send({"error": "unknown app"}, 403)
        elif self.path == "/ask":
            q = data.get("q") or ""
            status = {"battery": battery(), "brightness": brightness_get(),
                      "net": net_up(), "os": "AuroraOS"}
            env = {**os.environ,
                   "WAYLAND_DISPLAY": os.environ.get("WAYLAND_DISPLAY", "wayland-0"),
                   "MOZ_ENABLE_WAYLAND": "1"}

            def _spawn(cmd, shell=False):
                subprocess.Popen(cmd, shell=shell, env=env,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

            def _find_app(name):
                name = (name or "").strip().lower()
                if not name:
                    return None
                if name in ("terminal", "term", "console", "shell", "foot"):
                    return {"name": "Terminal", "exec": "foot"}
                apps = discover_apps()
                for v in apps.values():
                    if v["name"].strip().lower() == name:
                        return v
                for v in apps.values():
                    if name in v["name"].strip().lower():
                        return v
                return None

            def _open_terminal(a):
                _spawn(["foot"]); return "Opening a terminal."

            def _open_app(a):
                want = a.get("name") or a.get("app") or ""
                info = _find_app(want)
                if not info:
                    return f"I couldn't find an app called '{want}'."
                _spawn(info["exec"], shell=isinstance(info["exec"], str))
                return f"Opening {info['name']}."

            def _list_apps(a):
                names = sorted({v["name"] for v in discover_apps().values() if v.get("name")})
                return ("Installed apps: " + ", ".join(names)) if names else \
                       "Only the terminal is installed so far."

            def _power(a):
                act = a.get("action")
                if act in ("poweroff", "reboot"):
                    subprocess.Popen(["systemctl", act])
                    return "Shutting down…" if act == "poweroff" else "Restarting…"
                return "I can power off or restart — which would you like?"

            executors = {
                "open_terminal": _open_terminal,
                "open_app": _open_app,
                "list_apps": _list_apps,
                "power": _power,
                "set_brightness": lambda a: (f"Brightness set to {int(a.get('percent', 50))}%."
                                             if brightness_set(int(a.get("percent", 50)))
                                             else "This device has no software-controllable backlight."),
                "system_status": lambda a: self._status_line(status),
            }

            # Fast-path obvious imperative commands. The bundled 1B model is not
            # reliable at emitting tool-call JSON, so match clear intents directly
            # and only defer to the model for open-ended chat.
            ql = q.strip().lower()
            shortcut = None
            if re.search(r"\b(open|launch|start|new|run)\b.*\b(terminal|term|console|shell)\b", ql) \
               or ql in ("terminal", "cli"):
                shortcut = _open_terminal({})
            elif re.search(r"\b(list|show|what|which|installed|available)\b.*\bapps?\b", ql) \
                 or ql in ("apps", "applications"):
                shortcut = _list_apps({})
            elif re.search(r"\b(status|uptime|how'?s? (the )?(system|everything|things))\b", ql):
                shortcut = self._status_line(status)
            elif re.search(r"\bbattery\b", ql):
                shortcut = aura_llm.heuristic_fallback(q, status)
            elif re.search(r"\bbright", ql):
                m2 = re.search(r"(\d{1,3})", ql)
                shortcut = executors["set_brightness"](
                    {"percent": m2.group(1) if m2 else 50})
            elif re.search(r"\b(shut\s?down|power\s?off|turn\s?off)\b", ql):
                shortcut = _power({"action": "poweroff"})
            elif re.search(r"\b(restart|reboot)\b", ql):
                shortcut = _power({"action": "reboot"})
            else:
                m = re.match(r"(?:open|launch|start|run)\s+(?:the\s+|an?\s+)?(.+)", ql)
                if m:
                    info = _find_app(m.group(1).strip(" .!?'\""))
                    if info:
                        _spawn(info["exec"], shell=isinstance(info["exec"], str))
                        shortcut = f"Opening {info['name']}."

            if shortcut is not None:
                self._send({"a": shortcut, "actions": []})
            else:
                self._send(aura_llm.ask(q, executors=executors, status=status))
        else:
            self._send({"error": "not found"}, 404)

    def _status_line(self, status):
        b = status.get("battery") or {}
        batt = f"{b['percent']}% ({b['status']})" if b.get("percent") is not None else "AC power"
        return f"Battery {batt}; network {'up' if status.get('net') else 'down'}; {status.get('os')}."

    def log_message(self, *a): pass

if __name__ == "__main__":
    HTTPServer((os.environ.get("AURORAD_BIND", "127.0.0.1"), PORT), H).serve_forever()
