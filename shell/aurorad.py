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
import json, os, re, glob, subprocess, time, urllib.parse
import aura_llm
from http.server import HTTPServer, BaseHTTPRequestHandler

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

class H(BaseHTTPRequestHandler):
    def _send(self, obj, code=200):
        body = json.dumps(obj).encode()
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
        else:
            self._send({"error": "not found"}, 404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        try: data = json.loads(self.rfile.read(n) or b"{}")
        except json.JSONDecodeError: return self._send({"error": "bad json"}, 400)
        if self.path == "/brightness":
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
    HTTPServer(("127.0.0.1", PORT), H).serve_forever()
