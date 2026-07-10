# Aura Local LLM Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Aura's canned `/ask` responses with a bundled on-device LLM (Llama-3.2-1B-Instruct, Q4, via llama.cpp) that emits validated tool-calls into Aura's existing command registry, with deterministic fallback.

**Architecture:** A stdlib-only Python module `shell/aura_llm.py` builds a prompt from a shared tool registry (`config/aura-tools.json`), calls a localhost `llama-server`, parses tool-call JSON, validates against a whitelist, and routes each call to either server-side execution (brightness, status) or back to the browser (UI tools). `aurorad.py`'s `/ask` delegates to it; `aura-bridge.js` executes the browser-side actions against the in-page `COMMANDS` registry. If the model is down or returns garbage, it falls back to today's heuristic replies.

**Tech Stack:** Python 3 (stdlib only, to match `aurorad.py`), llama.cpp (`llama-server`, OpenAI-compatible `/v1/chat/completions`), GGUF model, JSON tool registry, vanilla JS shell. Tests: pytest on the host (`python -m pip install pytest`).

---

## Design constraints (read first)

- **`aura_llm.py` MUST be Python-3 stdlib only** — it ships to the LFS target, which has no pip. Tests may use pytest (they run on the host), but the module imports only `json, re, os, urllib.request, urllib.error`.
- **Tool sides.** Each tool in `config/aura-tools.json` has `"side": "system"` or `"side": "ui"`:
  - `system` (executed inside `aurorad`, response `ran:true`): `set_brightness`, `system_status`.
  - `ui` (returned for the browser, response `ran:false`): `open_app`, `close_app`, `tile_windows`, `set_theme`, `set_volume`, `toggle_setting`, `minimize_all`, `summarize_notifications`, `browse`, `search_files`, `show_panel`, `lock`, `capabilities`.
  - `power`/poweroff is deliberately **absent** from the registry — the model can never trigger it.
- **Registry is the single source of truth.** Tool *names* must match the `COMMANDS` keys already in `shell/index.html` (lines 722–763). Task 1 includes a parity test so they never drift.
- **Response contract** (extends today's `{"a": "..."}`, backward-compatible):
  ```json
  {"a": "<assembled prose reply>",
   "actions": [{"cmd": "set_brightness", "args": {"percent": 40}, "ran": true},
               {"cmd": "open_app", "args": {"app": "files"}, "ran": false}]}
  ```

## File structure

| File | Responsibility |
|---|---|
| `config/aura-tools.json` | **New.** Shared tool registry: name, side, description, arg schema. |
| `shell/aura_llm.py` | **New.** Prompt build, model call, parse, validate, route, fallback. Stdlib only. The testable core. |
| `tests/test_aura_tools.py` | **New.** Registry schema valid + names match `index.html` COMMANDS. |
| `tests/test_aura_llm.py` | **New.** Unit tests for every `aura_llm` function (mocked model). |
| `tests/aura_intents.jsonl` | **New.** Utterance→expected-cmd fixtures for the live smoke test. |
| `tests/test_aura_llm_live.sh` | **New.** Optional live smoke vs a running `llama-server` (skips if absent). |
| `shell/aurorad.py` | **Modify.** `/ask` delegates to `aura_llm.ask` with real system executors. |
| `shell/index.html` | **Modify.** Expose `Aura.exec(cmd,args)` and `Aura.toolNames()`. |
| `shell/aura-bridge.js` | **Modify.** Execute `ran:false` actions via `Aura.exec`. |
| `config/extras.list` | **Modify.** Add llama.cpp release URL. |
| `scripts/02-download-sources.sh` | **Modify.** Fetch llama.cpp tarball + GGUF model, integrity-check. |
| `scripts/10-aurora-shell.sh` | **Modify.** Build `llama-server`, install model + registry, write `aura-llm.service`. |

---

### Task 1: Tool registry + parity test

**Files:**
- Create: `config/aura-tools.json`
- Create: `tests/test_aura_tools.py`

- [ ] **Step 1: Write the failing test**

```python
# tests/test_aura_tools.py
import json, re, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS = json.loads((ROOT / "config/aura-tools.json").read_text(encoding="utf-8"))

VALID_SIDES = {"system", "ui"}

def test_every_tool_has_required_fields():
    for t in TOOLS:
        assert set(t) >= {"name", "side", "description", "args"}, t
        assert t["side"] in VALID_SIDES, t
        assert isinstance(t["args"], dict)

def test_no_power_tool_exposed():
    names = {t["name"] for t in TOOLS}
    assert "power" not in names and "poweroff" not in names and "reboot" not in names

def test_names_match_index_commands():
    """Every registry name must be a real COMMANDS key in index.html (no drift)."""
    html = (ROOT / "shell/index.html").read_text(encoding="utf-8")
    block = html[html.index("const COMMANDS={"): html.index("// ordered rules")]
    command_keys = set(re.findall(r"(\w+):\(", block))
    for t in TOOLS:
        assert t["name"] in command_keys, f"{t['name']} not in index.html COMMANDS"
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd "Desktop/Other Projects/auroraos" && python -m pytest tests/test_aura_tools.py -v`
Expected: FAIL — `FileNotFoundError: config/aura-tools.json`

- [ ] **Step 3: Create the registry**

```json
[
  {"name": "open_app", "side": "ui", "description": "Open an app window (files, edge, notes, settings).",
   "args": {"app": "one of: files, edge, notes, settings"}},
  {"name": "close_app", "side": "ui", "description": "Close an open app window.",
   "args": {"app": "one of: files, edge, notes, settings"}},
  {"name": "tile_windows", "side": "ui", "description": "Tile the open windows.",
   "args": {"layout": "one of: even, thirds, coding"}},
  {"name": "set_theme", "side": "ui", "description": "Switch light/dark theme.",
   "args": {"mode": "one of: light, dark, toggle"}},
  {"name": "set_brightness", "side": "system", "description": "Set screen brightness on real hardware.",
   "args": {"percent": "integer 0-100"}},
  {"name": "set_volume", "side": "ui", "description": "Set output volume or mute.",
   "args": {"percent": "integer 0-100", "mute": "boolean, optional"}},
  {"name": "toggle_setting", "side": "ui", "description": "Toggle a quick setting (wi-fi, bluetooth, airplane, focus, night light).",
   "args": {"name": "the setting name", "on": "boolean"}},
  {"name": "minimize_all", "side": "ui", "description": "Minimize every window and show the desktop.", "args": {}},
  {"name": "summarize_notifications", "side": "ui", "description": "Summarize and open the notifications panel.", "args": {}},
  {"name": "browse", "side": "ui", "description": "Open the browser, optionally to a URL or query.",
   "args": {"q": "a URL or search text, optional"}},
  {"name": "search_files", "side": "ui", "description": "Open Files and search the home folder.",
   "args": {"query": "search text, optional"}},
  {"name": "show_panel", "side": "ui", "description": "Open a shell panel (noti, qs, widgets).",
   "args": {"panel": "one of: noti, qs, widgets"}},
  {"name": "lock", "side": "ui", "description": "Lock the screen.", "args": {}},
  {"name": "system_status", "side": "system", "description": "Report battery, network, and uptime from real hardware.", "args": {}},
  {"name": "capabilities", "side": "ui", "description": "Explain what Aura can do.", "args": {}}
]
```

- [ ] **Step 4: Run to verify it passes**

Run: `python -m pytest tests/test_aura_tools.py -v`
Expected: PASS (3 tests)

- [ ] **Step 5: Commit**

```bash
git add config/aura-tools.json tests/test_aura_tools.py
git commit -m "feat(aura): shared tool registry + parity test with shell COMMANDS"
```

---

### Task 2: `load_tools` + `build_prompt`

**Files:**
- Create: `shell/aura_llm.py`
- Create: `tests/test_aura_llm.py`

- [ ] **Step 1: Write the failing test**

```python
# tests/test_aura_llm.py
import pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "shell"))
import aura_llm

def test_load_tools_reads_registry():
    tools = aura_llm.load_tools()
    names = {t["name"] for t in tools}
    assert "set_brightness" in names and "open_app" in names

def test_build_prompt_lists_tools_and_forbids_invention():
    tools = aura_llm.load_tools()
    system, user = aura_llm.build_prompt(tools, "open files")
    assert "set_brightness" in system and "open_app" in system
    assert "Never invent" in system
    assert user == "open files"
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -m pytest tests/test_aura_llm.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'aura_llm'`

- [ ] **Step 3: Create the module core**

```python
# shell/aura_llm.py
"""Aura's on-device LLM layer: prompt -> llama.cpp -> validated tool calls.
Stdlib only (ships to the LFS target). aurorad.py calls ask()."""
import json, os, re, urllib.request, urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS_PATH = os.environ.get("AURA_TOOLS", os.path.join(HERE, "..", "config", "aura-tools.json"))
LLAMA_URL = os.environ.get("AURA_LLM_URL", "http://127.0.0.1:8080/v1/chat/completions")
LLAMA_TIMEOUT = float(os.environ.get("AURA_LLM_TIMEOUT", "8"))

def load_tools(path=None):
    with open(path or TOOLS_PATH, encoding="utf-8") as f:
        return json.load(f)

def build_prompt(tools, user_text):
    lines = []
    for t in tools:
        args = ", ".join(f"{k} ({v})" for k, v in t["args"].items()) or "none"
        lines.append(f'- {t["name"]}: {t["description"]} args: {args}')
    system = (
        "You are Aura, the on-device assistant for AuroraOS. You either answer in "
        "prose, or perform actions by emitting a JSON object on its own line:\n"
        '{"reply": "<short confirmation>", "tool_calls": [{"cmd": "<tool>", "args": {...}}]}\n'
        "Tools you may call:\n" + "\n".join(lines) + "\n"
        "Only use listed tools with listed args. Never invent tools or arguments. "
        "If the user is just chatting, answer normally with no JSON."
    )
    return system, user_text
```

- [ ] **Step 4: Run to verify it passes**

Run: `python -m pytest tests/test_aura_llm.py -v`
Expected: PASS (2 tests)

- [ ] **Step 5: Commit**

```bash
git add shell/aura_llm.py tests/test_aura_llm.py
git commit -m "feat(aura): load_tools + build_prompt for the LLM layer"
```

---

### Task 3: `parse_model_output`

**Files:**
- Modify: `shell/aura_llm.py`
- Modify: `tests/test_aura_llm.py`

- [ ] **Step 1: Write the failing test**

```python
# append to tests/test_aura_llm.py
def test_parse_extracts_tool_calls():
    out = '{"reply":"On it.","tool_calls":[{"cmd":"open_app","args":{"app":"files"}}]}'
    r = aura_llm.parse_model_output(out)
    assert r["reply"] == "On it."
    assert r["tool_calls"] == [{"cmd": "open_app", "args": {"app": "files"}}]

def test_parse_json_embedded_in_prose():
    out = 'Sure!\n{"reply":"Opening.","tool_calls":[{"cmd":"browse","args":{"q":"bbc.com"}}]}\nDone.'
    r = aura_llm.parse_model_output(out)
    assert r["tool_calls"][0]["cmd"] == "browse"

def test_parse_plain_chat_has_no_tool_calls():
    r = aura_llm.parse_model_output("The capital of France is Paris.")
    assert r["tool_calls"] == []
    assert "Paris" in r["reply"]

def test_parse_malformed_json_degrades_to_chat():
    r = aura_llm.parse_model_output('{"reply": "oops", "tool_calls": [broken')
    assert r["tool_calls"] == []
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -m pytest tests/test_aura_llm.py -k parse -v`
Expected: FAIL — `AttributeError: module 'aura_llm' has no attribute 'parse_model_output'`

- [ ] **Step 3: Implement**

```python
# add to shell/aura_llm.py
def parse_model_output(text):
    """Return {'reply': str, 'tool_calls': [{'cmd','args'}]}. Never raises."""
    text = (text or "").strip()
    for m in re.finditer(r"\{.*\}", text, re.DOTALL):
        try:
            obj = json.loads(m.group(0))
        except (ValueError, TypeError):
            continue
        if isinstance(obj, dict) and "tool_calls" in obj:
            calls = obj.get("tool_calls") or []
            if not isinstance(calls, list):
                calls = []
            clean = [{"cmd": c.get("cmd"), "args": c.get("args") or {}}
                     for c in calls if isinstance(c, dict) and c.get("cmd")]
            return {"reply": str(obj.get("reply") or "").strip(), "tool_calls": clean}
    return {"reply": text, "tool_calls": []}
```

- [ ] **Step 4: Run to verify it passes**

Run: `python -m pytest tests/test_aura_llm.py -k parse -v`
Expected: PASS (4 tests)

- [ ] **Step 5: Commit**

```bash
git add shell/aura_llm.py tests/test_aura_llm.py
git commit -m "feat(aura): tolerant parse_model_output (json tool-calls or prose)"
```

---

### Task 4: `validate_call` (whitelist enforcement)

**Files:**
- Modify: `shell/aura_llm.py`
- Modify: `tests/test_aura_llm.py`

- [ ] **Step 1: Write the failing test**

```python
# append to tests/test_aura_llm.py
def test_validate_accepts_known_tool_with_known_args():
    tools = aura_llm.load_tools()
    assert aura_llm.validate_call({"cmd": "set_brightness", "args": {"percent": 40}}, tools)

def test_validate_rejects_unknown_tool():
    tools = aura_llm.load_tools()
    assert not aura_llm.validate_call({"cmd": "delete_everything", "args": {}}, tools)

def test_validate_rejects_power_even_if_model_emits_it():
    tools = aura_llm.load_tools()
    assert not aura_llm.validate_call({"cmd": "power", "args": {"action": "poweroff"}}, tools)

def test_validate_rejects_unknown_arg_keys():
    tools = aura_llm.load_tools()
    assert not aura_llm.validate_call({"cmd": "open_app", "args": {"rm": "-rf"}}, tools)
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -m pytest tests/test_aura_llm.py -k validate -v`
Expected: FAIL — no attribute `validate_call`

- [ ] **Step 3: Implement**

```python
# add to shell/aura_llm.py
def _tool_index(tools):
    return {t["name"]: t for t in tools}

def validate_call(call, tools):
    """True only if cmd is a registry tool and every arg key is declared."""
    idx = _tool_index(tools)
    spec = idx.get(call.get("cmd"))
    if not spec:
        return False
    allowed = set(spec["args"].keys())
    return all(k in allowed for k in (call.get("args") or {}))
```

- [ ] **Step 4: Run to verify it passes**

Run: `python -m pytest tests/test_aura_llm.py -k validate -v`
Expected: PASS (4 tests)

- [ ] **Step 5: Commit**

```bash
git add shell/aura_llm.py tests/test_aura_llm.py
git commit -m "feat(aura): validate_call whitelist (reject unknown/power/bad-args)"
```

---

### Task 5: `route` (system-side execute vs UI-side defer)

**Files:**
- Modify: `shell/aura_llm.py`
- Modify: `tests/test_aura_llm.py`

- [ ] **Step 1: Write the failing test**

```python
# append to tests/test_aura_llm.py
def test_route_executes_system_tool_and_marks_ran():
    tools = aura_llm.load_tools()
    calls = []
    execs = {"set_brightness": lambda args: calls.append(args) or "brightness 40%"}
    actions, notes = aura_llm.route(
        [{"cmd": "set_brightness", "args": {"percent": 40}}], tools, execs)
    assert calls == [{"percent": 40}]
    assert actions == [{"cmd": "set_brightness", "args": {"percent": 40}, "ran": True}]
    assert "brightness 40%" in notes[0]

def test_route_defers_ui_tool_unrun():
    tools = aura_llm.load_tools()
    actions, notes = aura_llm.route(
        [{"cmd": "open_app", "args": {"app": "files"}}], tools, {})
    assert actions == [{"cmd": "open_app", "args": {"app": "files"}, "ran": False}]

def test_route_drops_invalid_calls():
    tools = aura_llm.load_tools()
    actions, notes = aura_llm.route([{"cmd": "power", "args": {}}], tools, {})
    assert actions == []
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -m pytest tests/test_aura_llm.py -k route -v`
Expected: FAIL — no attribute `route`

- [ ] **Step 3: Implement**

```python
# add to shell/aura_llm.py
def route(calls, tools, executors):
    """Execute system-side tools now; mark UI-side tools for the browser.
    Returns (actions, notes) where notes are server-side result strings."""
    idx = _tool_index(tools)
    actions, notes = [], []
    for call in calls:
        if not validate_call(call, tools):
            continue
        spec = idx[call["cmd"]]
        args = call.get("args") or {}
        if spec["side"] == "system":
            fn = executors.get(call["cmd"])
            note = fn(args) if fn else None
            if note:
                notes.append(note)
            actions.append({"cmd": call["cmd"], "args": args, "ran": True})
        else:
            actions.append({"cmd": call["cmd"], "args": args, "ran": False})
    return actions, notes
```

- [ ] **Step 4: Run to verify it passes**

Run: `python -m pytest tests/test_aura_llm.py -k route -v`
Expected: PASS (3 tests)

- [ ] **Step 5: Commit**

```bash
git add shell/aura_llm.py tests/test_aura_llm.py
git commit -m "feat(aura): route system-side execution vs UI-side deferral"
```

---

### Task 6: `heuristic_fallback` (move today's canned logic)

**Files:**
- Modify: `shell/aura_llm.py`
- Modify: `tests/test_aura_llm.py`

- [ ] **Step 1: Write the failing test**

```python
# append to tests/test_aura_llm.py
def test_fallback_reports_battery():
    r = aura_llm.heuristic_fallback("what's my battery",
                                    status={"battery": {"percent": 88, "status": "Discharging"}})
    assert "88%" in r

def test_fallback_default_message():
    r = aura_llm.heuristic_fallback("tell me a joke", status={})
    assert "battery" in r.lower() or "status" in r.lower()
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -m pytest tests/test_aura_llm.py -k fallback -v`
Expected: FAIL — no attribute `heuristic_fallback`

- [ ] **Step 3: Implement**

```python
# add to shell/aura_llm.py
def heuristic_fallback(user_text, status=None):
    """Deterministic reply when the model is unavailable. Mirrors the old /ask."""
    qs = (user_text or "").lower()
    status = status or {}
    if "battery" in qs:
        b = status.get("battery") or {}
        pct = b.get("percent")
        return (f"Battery is at {pct}% ({b.get('status','')})." if pct is not None
                else "No battery — running on AC power.")
    if "bright" in qs:
        return f"Brightness is {status.get('brightness') or 'not controllable on this device'}%."
    if any(k in qs for k in ("off", "shutdown", "power")):
        return "Use the start-menu power button to shut down or restart."
    return ("I can report battery, brightness, and system status, and control the "
            "desktop — try 'open files' or 'set brightness to 40'.")
```

- [ ] **Step 4: Run to verify it passes**

Run: `python -m pytest tests/test_aura_llm.py -k fallback -v`
Expected: PASS (2 tests)

- [ ] **Step 5: Commit**

```bash
git add shell/aura_llm.py tests/test_aura_llm.py
git commit -m "feat(aura): heuristic_fallback ported from the old /ask stub"
```

---

### Task 7: `call_llama` (HTTP client, injectable)

**Files:**
- Modify: `shell/aura_llm.py`
- Modify: `tests/test_aura_llm.py`

- [ ] **Step 1: Write the failing test**

```python
# append to tests/test_aura_llm.py
def test_call_llama_posts_messages_and_reads_content(monkeypatch):
    captured = {}
    class FakeResp:
        def read(self): return b'{"choices":[{"message":{"content":"hi there"}}]}'
        def __enter__(self): return self
        def __exit__(self, *a): return False
    def fake_urlopen(req, timeout=None):
        captured["body"] = req.data
        return FakeResp()
    monkeypatch.setattr(aura_llm.urllib.request, "urlopen", fake_urlopen)
    out = aura_llm.call_llama("SYS", "hello")
    assert out == "hi there"
    body = aura_llm.json.loads(captured["body"])
    assert body["messages"][0]["role"] == "system"
    assert body["messages"][1]["content"] == "hello"

def test_call_llama_returns_none_on_connection_error(monkeypatch):
    def boom(req, timeout=None):
        raise aura_llm.urllib.error.URLError("refused")
    monkeypatch.setattr(aura_llm.urllib.request, "urlopen", boom)
    assert aura_llm.call_llama("SYS", "hello") is None
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -m pytest tests/test_aura_llm.py -k call_llama -v`
Expected: FAIL — no attribute `call_llama`

- [ ] **Step 3: Implement**

```python
# add to shell/aura_llm.py
def call_llama(system, user):
    """POST to llama-server; return assistant content or None on any failure."""
    body = json.dumps({
        "messages": [{"role": "system", "content": system},
                     {"role": "user", "content": user}],
        "temperature": 0.2,
        "max_tokens": 256,
    }).encode()
    req = urllib.request.Request(LLAMA_URL, data=body,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=LLAMA_TIMEOUT) as r:
            data = json.loads(r.read())
        return data["choices"][0]["message"]["content"]
    except (urllib.error.URLError, OSError, ValueError, KeyError, IndexError):
        return None
```

- [ ] **Step 4: Run to verify it passes**

Run: `python -m pytest tests/test_aura_llm.py -k call_llama -v`
Expected: PASS (2 tests)

- [ ] **Step 5: Commit**

```bash
git add shell/aura_llm.py tests/test_aura_llm.py
git commit -m "feat(aura): call_llama HTTP client with graceful failure"
```

---

### Task 8: `ask` orchestrator

**Files:**
- Modify: `shell/aura_llm.py`
- Modify: `tests/test_aura_llm.py`

- [ ] **Step 1: Write the failing test**

```python
# append to tests/test_aura_llm.py
def _tools(): return aura_llm.load_tools()

def test_ask_happy_path_executes_and_returns_actions(monkeypatch):
    monkeypatch.setattr(aura_llm, "call_llama",
        lambda s, u: '{"reply":"Opening Files.","tool_calls":[{"cmd":"open_app","args":{"app":"files"}}]}')
    out = aura_llm.ask("open files", executors={}, status={})
    assert out["actions"] == [{"cmd": "open_app", "args": {"app": "files"}, "ran": False}]
    assert "Opening Files" in out["a"]

def test_ask_merges_system_notes_into_reply(monkeypatch):
    monkeypatch.setattr(aura_llm, "call_llama",
        lambda s, u: '{"reply":"Done.","tool_calls":[{"cmd":"set_brightness","args":{"percent":40}}]}')
    execs = {"set_brightness": lambda a: "brightness set to 40%"}
    out = aura_llm.ask("dim to 40", executors=execs, status={})
    assert out["actions"][0]["ran"] is True
    assert "brightness set to 40%" in out["a"]

def test_ask_falls_back_when_model_down(monkeypatch):
    monkeypatch.setattr(aura_llm, "call_llama", lambda s, u: None)
    out = aura_llm.ask("what's my battery",
                       executors={}, status={"battery": {"percent": 55, "status": "Full"}})
    assert "55%" in out["a"]
    assert out["actions"] == []

def test_ask_falls_back_on_garbage(monkeypatch):
    monkeypatch.setattr(aura_llm, "call_llama", lambda s, u: "%%% not json %%%")
    out = aura_llm.ask("hello", executors={}, status={})
    assert out["actions"] == []
    assert out["a"]  # non-empty prose (the model's own text)
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -m pytest tests/test_aura_llm.py -k ask -v`
Expected: FAIL — no attribute `ask`

- [ ] **Step 3: Implement**

```python
# add to shell/aura_llm.py
def ask(user_text, executors=None, status=None, tools=None):
    """Full pipeline: model -> parse -> validate/route -> reply. Always returns
    {'a': str, 'actions': [...]}. Falls back to heuristics if the model fails."""
    executors = executors or {}
    tools = tools if tools is not None else load_tools()
    system, user = build_prompt(tools, user_text)
    raw = call_llama(system, user)
    if raw is None:
        return {"a": heuristic_fallback(user_text, status), "actions": []}
    parsed = parse_model_output(raw)
    actions, notes = route(parsed["tool_calls"], tools, executors)
    reply = parsed["reply"] or ""
    if notes:
        reply = (reply + " " + " ".join(notes)).strip()
    if not reply:
        reply = heuristic_fallback(user_text, status)
    return {"a": reply, "actions": actions}
```

- [ ] **Step 4: Run to verify it passes**

Run: `python -m pytest tests/test_aura_llm.py -k ask -v`
Expected: PASS (4 tests)

- [ ] **Step 5: Commit**

```bash
git add shell/aura_llm.py tests/test_aura_llm.py
git commit -m "feat(aura): ask() orchestrator with model->tools->fallback pipeline"
```

---

### Task 9: Wire `aurorad.py` `/ask` to `aura_llm`

**Files:**
- Modify: `shell/aurorad.py:14` (imports), `shell/aurorad.py:176-189` (`/ask` handler)
- Create: `tests/test_aurorad_ask.py`

- [ ] **Step 1: Write the failing test**

```python
# tests/test_aurorad_ask.py — black-box test hitting a live aurorad with a stub model
import json, os, subprocess, sys, time, urllib.request, pathlib, socket

ROOT = pathlib.Path(__file__).resolve().parents[1]

def _free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def test_ask_returns_actions_with_stub_model(tmp_path):
    # a fake llama-server that always asks to open files
    port = _free_port()
    stub = tmp_path / "stub.py"
    stub.write_text(
        "import json\n"
        "from http.server import HTTPServer, BaseHTTPRequestHandler\n"
        "class H(BaseHTTPRequestHandler):\n"
        "  def do_POST(self):\n"
        "    n=int(self.headers.get('Content-Length',0)); self.rfile.read(n)\n"
        "    b=json.dumps({'choices':[{'message':{'content':'{\\\"reply\\\":\\\"Opening.\\\",\\\"tool_calls\\\":[{\\\"cmd\\\":\\\"open_app\\\",\\\"args\\\":{\\\"app\\\":\\\"files\\\"}}]}'}}]}).encode()\n"
        "    self.send_response(200); self.send_header('Content-Length',str(len(b))); self.end_headers(); self.wfile.write(b)\n"
        "  def log_message(self,*a): pass\n"
        f"HTTPServer(('127.0.0.1',{port}),H).serve_forever()\n")
    sm = subprocess.Popen([sys.executable, str(stub)])
    dport = _free_port()
    env = {**os.environ,
           "AURA_LLM_URL": f"http://127.0.0.1:{port}/v1/chat/completions",
           "AURORAD_PORT": str(dport)}
    dm = subprocess.Popen([sys.executable, str(ROOT / "shell/aurorad.py")], env=env)
    try:
        time.sleep(1.0)
        req = urllib.request.Request(f"http://127.0.0.1:{dport}/ask",
                                     data=json.dumps({"q": "open files"}).encode(),
                                     headers={"Content-Type": "application/json"})
        out = json.loads(urllib.request.urlopen(req, timeout=5).read())
        assert out["actions"][0]["cmd"] == "open_app"
        assert out["actions"][0]["ran"] is False
    finally:
        dm.terminate(); sm.terminate()
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -m pytest tests/test_aurorad_ask.py -v`
Expected: FAIL — `aurorad.py` ignores `AURA_LLM_URL`/`AURORAD_PORT`, `/ask` still returns only `{"a":...}` with no `actions`.

- [ ] **Step 3: Edit `aurorad.py`**

Change the import line (`shell/aurorad.py:14`) to add the module and make the port overridable:

```python
import json, os, re, glob, subprocess, time, urllib.parse
import aura_llm
from http.server import HTTPServer, BaseHTTPRequestHandler

PORT = int(os.environ.get("AURORAD_PORT", "7212"))
```

Replace the whole `elif self.path == "/ask":` block (`shell/aurorad.py:176-189`) with:

```python
        elif self.path == "/ask":
            q = data.get("q") or ""
            status = {"battery": battery(), "brightness": brightness_get(),
                      "net": net_up(), "os": "AuroraOS"}
            executors = {
                "set_brightness": lambda a: (brightness_set(int(a.get("percent", 50)))
                                             and f"Brightness set to {int(a.get('percent',50))}%."),
                "system_status": lambda a: self._status_line(status),
            }
            self._send(aura_llm.ask(q, executors=executors, status=status))
```

Add a small helper method to class `H` (just above `def log_message`):

```python
    def _status_line(self, status):
        b = status.get("battery") or {}
        batt = f"{b['percent']}% ({b['status']})" if b.get("percent") is not None else "AC power"
        return f"Battery {batt}; network {'up' if status.get('net') else 'down'}; {status.get('os')}."
```

And update the bottom to honor the port:

```python
if __name__ == "__main__":
    HTTPServer(("127.0.0.1", PORT), H).serve_forever()
```

- [ ] **Step 4: Run to verify it passes**

Run: `python -m pytest tests/test_aurorad_ask.py -v`
Expected: PASS

Also run the whole suite: `python -m pytest tests/ -v` → all green.

- [ ] **Step 5: Commit**

```bash
git add shell/aurorad.py tests/test_aurorad_ask.py
git commit -m "feat(aura): aurorad /ask delegates to aura_llm with real executors"
```

---

### Task 10: Expose `Aura.exec` in the shell

**Files:**
- Modify: `shell/index.html:794` (the `Aura` return object)

- [ ] **Step 1: Add an exec entry point**

The bridge needs to run a *named* command with args (not re-parse text). Change the `Aura` return (line 794) from:

```javascript
  return{interpret,run,COMMANDS,audit:AUDIT};
```

to:

```javascript
  function exec(cmd,args){return COMMANDS[cmd]?COMMANDS[cmd](args||{}):`Unknown action: ${cmd}.`;}
  function toolNames(){return Object.keys(COMMANDS);}
  return{interpret,run,exec,toolNames,COMMANDS,audit:AUDIT};
```

- [ ] **Step 2: Verify in a browser (manual, no runner available)**

Open `shell/index.html` directly, open the console, run:
`Aura.exec("set_theme",{mode:"toggle"})`
Expected: theme flips, returns a confirmation string. `Aura.toolNames()` lists the command keys.

- [ ] **Step 3: Commit**

```bash
git add shell/index.html
git commit -m "feat(aura): expose Aura.exec(cmd,args) for bridge-driven tool calls"
```

---

### Task 11: Bridge executes `ran:false` actions

**Files:**
- Modify: `shell/aura-bridge.js:83-93` (the `cpAsk` override)

- [ ] **Step 1: Replace the `/ask` override**

Replace the current `origAsk`/`cpAsk` block (`shell/aura-bridge.js:83-93`) with:

```javascript
  /* ---- Aura assistant: aurorad runs the model + system tools; we run UI tools ---- */
  const origAsk = window.cpAsk;
  if (typeof origAsk === "function") {
    window.cpAsk = function (q) {
      if (!live) return origAsk(q);
      window.cpMsg(q, "u");
      j("/ask", { method: "POST", body: JSON.stringify({ q }) })
        .then((d) => {
          (d.actions || []).forEach((a) => {
            if (!a.ran && window.Aura && window.Aura.exec) window.Aura.exec(a.cmd, a.args);
          });
          window.cpMsg(d.a || "…", "a");
        })
        .catch(() => window.cpReply(q.toLowerCase()));
    };
  }
```

- [ ] **Step 2: Verify (manual)**

With a running system (or a stubbed `/ask` returning an `open_app` action with `ran:false`), type "open files" in the Aura panel. Expected: the Files window opens AND the reply prints. With `aurorad` offline, typing still yields the local heuristic reply (`origAsk`).

- [ ] **Step 3: Commit**

```bash
git add shell/aura-bridge.js
git commit -m "feat(aura): bridge executes UI-side tool calls from /ask actions"
```

---

### Task 12: Fetch llama.cpp + model in the download stage

**Files:**
- Modify: `config/extras.list` (add llama.cpp under a new section)
- Modify: `scripts/02-download-sources.sh` (model fetch + integrity check)

- [ ] **Step 1: Add llama.cpp to `extras.list`**

Append this section to `config/extras.list`:

```
# --- Aura on-device LLM (llama.cpp; model fetched separately in script 02) ---
https://github.com/ggerganov/llama.cpp/archive/refs/tags/b4589.tar.gz
```

- [ ] **Step 2: Add a model-fetch block to `scripts/02-download-sources.sh`**

At the end of `scripts/02-download-sources.sh` (before any final "done" echo), add:

```bash
# ---- Aura LLM model (bundled, offline-first) ----
AURA_MODEL_DIR="$LFS/opt/aura/models"
AURA_MODEL="Llama-3.2-1B-Instruct-Q4_K_M.gguf"
AURA_MODEL_URL="https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF/resolve/main/${AURA_MODEL}"
mkdir -p "$AURA_MODEL_DIR"
if [ ! -f "$AURA_MODEL_DIR/$AURA_MODEL" ]; then
  echo "==== downloading Aura model ($AURA_MODEL, ~0.8 GB) ===="
  wget --timeout=30 --tries=5 -O "$AURA_MODEL_DIR/$AURA_MODEL" "$AURA_MODEL_URL"
fi
# Integrity: GGUF files start with the ASCII magic "GGUF"; reject truncated/HTML error pages.
if [ "$(head -c 4 "$AURA_MODEL_DIR/$AURA_MODEL")" != "GGUF" ]; then
  echo "!! Aura model is not a valid GGUF (download failed?). Removing."; rm -f "$AURA_MODEL_DIR/$AURA_MODEL"; exit 1
fi
sz=$(stat -c%s "$AURA_MODEL_DIR/$AURA_MODEL")
[ "$sz" -gt 500000000 ] || { echo "!! Aura model too small ($sz bytes) — incomplete."; exit 1; }
echo "Aura model OK ($sz bytes)."
```

- [ ] **Step 3: Verify (host, dry sanity)**

Run: `bash -n scripts/02-download-sources.sh`
Expected: no syntax errors. (Full download runs during a real build.)

- [ ] **Step 4: Commit**

```bash
git add config/extras.list scripts/02-download-sources.sh
git commit -m "feat(aura): fetch llama.cpp source + bundle Llama-3.2-1B GGUF with GGUF-magic check"
```

---

### Task 13: Build `llama-server` + install service in script 10

**Files:**
- Modify: `scripts/10-aurora-shell.sh` (build step + service unit + registry install)

- [ ] **Step 1: Add the llama.cpp build near the end of script 10**

After the existing extras builds (and before the final services section), add:

```bash
# ==== Aura on-device LLM: build llama-server, install model + registry ====
if [ ! -f $STAMPS/x-llama ]; then
  echo "==== extras: llama.cpp (cmake) ===="
  tb=$(ls llama.cpp-*.tar.gz 2>/dev/null | head -1)
  xt "$tb"
  cmake -B build -DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=ON \
        -DLLAMA_CURL=OFF -DBUILD_SHARED_LIBS=OFF -DLLAMA_BUILD_SERVER=ON
  cmake --build build --config Release -j"$(nproc)" --target llama-server
  install -Dm755 build/bin/llama-server /opt/aura/bin/llama-server
  fin; touch $STAMPS/x-llama
fi

# aura assets: the shared tool registry + the LLM module live next to the shell
install -Dm644 /sources/aurora/config/aura-tools.json /opt/aura/config/aura-tools.json
install -Dm644 /sources/aurora/shell/aura_llm.py /opt/aura/shell/aura_llm.py
```

> Note: `/sources/aurora` is where script 05/06 stages this repo inside the chroot (the same path the shell files are copied from). If your tree stages the repo elsewhere, match that path — grep script 10 for where `index.html` is copied and reuse that root.

- [ ] **Step 2: Point aurorad at the installed registry**

Ensure the `aurorad.service` (written later in script 10) sets the tools path and starts the model first. Add/adjust the service definitions section so it contains:

```bash
cat > /etc/systemd/system/aura-llm.service <<'EOF'
[Unit]
Description=Aura on-device LLM (llama.cpp)
After=network.target
[Service]
User=aurora
ExecStart=/opt/aura/bin/llama-server --model /opt/aura/models/Llama-3.2-1B-Instruct-Q4_K_M.gguf --host 127.0.0.1 --port 8080 --ctx-size 4096
Restart=on-failure
[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/aurorad.service <<'EOF'
[Unit]
Description=AuroraOS system bridge
After=network.target aura-llm.service
Wants=aura-llm.service
[Service]
User=aurora
Environment=AURA_TOOLS=/opt/aura/config/aura-tools.json
Environment=AURA_LLM_URL=http://127.0.0.1:8080/v1/chat/completions
ExecStart=/usr/bin/python3 /opt/aura/shell/aurorad.py
Restart=on-failure
[Install]
WantedBy=multi-user.target
EOF

systemctl enable aura-llm.service aurorad.service
```

> If script 10 already writes an `aurorad.service`, replace that block with the two units above rather than duplicating it. `aura_llm.py` resolves `AURA_TOOLS` from the env, so it finds the registry regardless of layout.

- [ ] **Step 3: Verify (host, syntax only)**

Run: `bash -n scripts/10-aurora-shell.sh`
Expected: no syntax errors.

- [ ] **Step 4: Commit**

```bash
git add scripts/10-aurora-shell.sh
git commit -m "feat(aura): build llama-server + aura-llm.service, wire aurorad env"
```

---

### Task 14: Prove llama.cpp builds on aarch64 (container)

**Files:** none (verification task using the existing `aurora-arm64` container).

- [ ] **Step 1: Build llama-server in the arm64 container**

```bash
wsl -d Ubuntu docker exec aurora-arm64 bash -c '
set -e; cd /tmp; rm -rf lc && mkdir lc && cd lc
apt-get install -y -qq cmake build-essential libgomp1 >/dev/null 2>&1
wget -q -O llama.tar.gz https://github.com/ggerganov/llama.cpp/archive/refs/tags/b4589.tar.gz
tar xf llama.tar.gz --strip-components=1
cmake -B build -DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=ON -DLLAMA_CURL=OFF -DBUILD_SHARED_LIBS=OFF -DLLAMA_BUILD_SERVER=ON >/tmp/lc.log 2>&1
cmake --build build -j"$(nproc)" --target llama-server >>/tmp/lc.log 2>&1
test -x build/bin/llama-server && echo LLAMA_ARM64_PASS || echo LLAMA_ARM64_FAIL'
```

Expected: `LLAMA_ARM64_PASS`. If it fails, read `/tmp/lc.log` in the container and bump the release tag or add the missing dep (mirrors the wlroots/libwayland lesson).

- [ ] **Step 2: Record the result**

Note PASS/FAIL and the tag used in the project memory (`auroraos_m4_project.md`). No commit (verification only).

---

### Task 15: Live intent fixtures + smoke script

**Files:**
- Create: `tests/aura_intents.jsonl`
- Create: `tests/test_aura_llm_live.sh`

- [ ] **Step 1: Create the fixture set**

```
{"say": "open the browser", "cmd": "browse"}
{"say": "switch to light mode", "cmd": "set_theme"}
{"say": "set brightness to 30", "cmd": "set_brightness"}
{"say": "tile my windows for coding", "cmd": "tile_windows"}
{"say": "turn off wi-fi", "cmd": "toggle_setting"}
{"say": "lock the screen", "cmd": "lock"}
{"say": "what did i miss", "cmd": "summarize_notifications"}
{"say": "minimize everything", "cmd": "minimize_all"}
{"say": "find my screenshots", "cmd": "search_files"}
{"say": "how's my battery", "cmd": "system_status"}
```

- [ ] **Step 2: Create the smoke script**

```bash
# tests/test_aura_llm_live.sh — runs the fixtures against a live llama-server.
# Skips (exit 0) if no server is reachable, so CI without a model still passes.
#!/bin/bash
set -u
URL="${AURA_LLM_URL:-http://127.0.0.1:8080/v1/chat/completions}"
curl -s -o /dev/null --max-time 2 "$URL" || { echo "SKIP: no llama-server at $URL"; exit 0; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
pass=0; total=0
while IFS= read -r line; do
  say=$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['say'])" "$line")
  want=$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['cmd'])" "$line")
  got=$(cd "$ROOT/shell" && python3 -c "
import json,aura_llm
o=aura_llm.ask('$say',executors={},status={})
print((o['actions'][0]['cmd'] if o['actions'] else 'CHAT'))")
  total=$((total+1)); [ "$got" = "$want" ] && pass=$((pass+1)) || echo "MISS: '$say' want=$want got=$got"
done < "$ROOT/tests/aura_intents.jsonl"
echo "intent match: $pass/$total"
[ "$pass" -ge $((total*9/10)) ]  # gate: >=90%
```

- [ ] **Step 3: Verify (host)**

Run: `bash tests/test_aura_llm_live.sh`
Expected without a model: `SKIP: no llama-server…` and exit 0. With a running `llama-server`: `intent match: N/10` and exit 0 if ≥9/10.

- [ ] **Step 4: Commit**

```bash
git add tests/aura_intents.jsonl tests/test_aura_llm_live.sh
git commit -m "test(aura): live intent fixtures + skippable smoke against llama-server"
```

---

## Final verification

- [ ] `python -m pytest tests/ -v` — all unit + integration tests green.
- [ ] `bash -n scripts/02-download-sources.sh scripts/10-aurora-shell.sh` — no shell syntax errors.
- [ ] Task 14 shows `LLAMA_ARM64_PASS`.
- [ ] Update `docs/CONTINUE-ON-MAC.md`: note the extra ~0.8 GB model download in script 02 and that `aura-llm.service` must be running for Aura's smart replies (heuristics otherwise).
- [ ] Update project memory `auroraos_m4_project.md` with the Aura-LLM feature state.

## Post-execution corrections (2026-07-10)

Built subagent-driven; two-stage review caught real defects fixed beyond the task text above. Recorded here so the plan matches the shipped code:

- **`aura_llm.py` hardening** (commit `c714fe3`): `call_llama` now `except Exception` (a non-dict JSON response raised an uncaught `TypeError`); executor calls in `route` are wrapped (an executor raising escaped `ask`'s "always returns" contract); `ran` is `True` only on a real successful execution (system tool with no executor was falsely `ran:true`); non-dict `args` are coerced/rejected; the greedy `\{.*\}` parse became a brace-depth scanner (`_json_candidates`) so a tool-call object followed by more JSON still parses.
- **`window.Aura` exposure** (commit `3780cd7`): `Aura` is a classic-script `const`, so `window.Aura` was `undefined` and the bridge silently skipped every UI action. `index.html` now does `window.Aura = Aura;` after the IIFE. (Task 10's code block above omitted this line — it is required.)
- **Bridge filename:** the real file is `shell/aurora-bridge.js` (not `aura-bridge.js` as written in Task 11 / the file table).
- **`aurorad.service`** (commit `9da4981`): ExecStart is `/usr/bin/python3 /usr/lib/aurora/aurorad` (aurorad installs there, not `/opt/aura/shell/aurorad.py`), and `aura_llm.py` installs to `/usr/lib/aurora/` next to it so `import aura_llm` resolves; the registry stays at `/opt/aura/config/` (found via `AURA_TOOLS`). aurorad runs as **root** (no `User=` line) — it writes the backlight and calls `systemctl`; only `aura-llm.service` runs as `User=aurora`.

## Self-review notes (author)

- **Spec coverage:** model choice → Tasks 12/13; llama.cpp runtime → 13/14; tool routing (server vs UI) → 5/9/11; shared registry → 1; prompt → 2; guardrails (whitelist, no power, fallback) → 4/6/8; bundled offline model → 12; systemd units → 13; tests (offline unit, fallback, whitelist, aarch64 build, in-VM) → 3–9/14/15. All spec sections mapped.
- **Deviation from spec, flagged:** spec listed `search_files` as "lean server-side"; this plan keeps it **UI-side** for v1 (no semantic-search engine exists yet — inventing one is out of scope). `set_volume` is UI-side (aurorad has no volume backend). Both are honest MVP calls; revisit in phase 2.
- **Brightness UI sync:** `set_brightness` runs server-side (`ran:true`); the shell slider catches up on the next 10 s `/status` poll rather than instantly. Acceptable for v1; a push-sync is a phase-2 nicety.
