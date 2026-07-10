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

def _json_candidates(text):
    """Yield each top-level {...} substring via a brace-depth scan, so multiple
    JSON objects in the text are tried independently (not greedily merged).
    String-literal edge cases (braces inside strings) are acceptable to ignore for v1."""
    depth = 0
    start = -1
    for i, ch in enumerate(text):
        if ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}" and depth > 0:
            depth -= 1
            if depth == 0 and start >= 0:
                yield text[start:i + 1]
                start = -1

def parse_model_output(text):
    """Return {'reply': str, 'tool_calls': [{'cmd','args'}]}. Never raises."""
    text = (text or "").strip()
    for candidate in _json_candidates(text):
        try:
            obj = json.loads(candidate)
        except (ValueError, TypeError):
            continue
        if isinstance(obj, dict) and "tool_calls" in obj:
            calls = obj.get("tool_calls") or []
            if not isinstance(calls, list):
                calls = []
            clean = [{"cmd": c.get("cmd"),
                      "args": c.get("args") if isinstance(c.get("args"), dict) else {}}
                     for c in calls if isinstance(c, dict) and c.get("cmd")]
            return {"reply": str(obj.get("reply") or "").strip(), "tool_calls": clean}
    return {"reply": text, "tool_calls": []}

def _tool_index(tools):
    return {t["name"]: t for t in tools}

def validate_call(call, tools):
    """True only if cmd is a registry tool and every arg key is declared.
    NOTE: this validates arg *keys* only. Executors MUST sanitize arg *values*
    (the registry's "integer 0-100" prose is descriptive, not enforced here)."""
    idx = _tool_index(tools)
    spec = idx.get(call.get("cmd"))
    if not spec:
        return False
    args = call.get("args")
    if args is None:
        args = {}
    if not isinstance(args, dict):
        return False
    allowed = set(spec["args"].keys())
    return all(k in allowed for k in args)

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
            ran = False
            if fn:
                try:
                    note = fn(args)
                    ran = True
                    if note:
                        notes.append(note)
                except Exception:
                    notes.append("Couldn't complete that action.")
            actions.append({"cmd": call["cmd"], "args": args, "ran": ran})
        else:
            actions.append({"cmd": call["cmd"], "args": args, "ran": False})
    return actions, notes

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
    except Exception:
        # Contract: return content or None on ANY failure (URLError, OSError,
        # IncompleteRead/HTTPException, non-dict JSON -> TypeError, missing keys, etc.).
        return None

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
