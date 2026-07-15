"""Aura's on-device LLM layer: prompt -> llama.cpp -> validated tool calls.
Stdlib only (ships to the LFS target). aurorad.py calls ask()."""
import json, os, re, urllib.request, urllib.error

# The bundled 1B model frequently hallucinates a tool call for plain chit-chat
# (e.g. "hi" -> open_terminal). A model-emitted action is only honored when the
# user's own words show action intent; otherwise the call is dropped and we just
# chat. Deterministic commands are already handled upstream in aurorad.py.
_ACTION_CUE = re.compile(
    r"\b(open|launch|start|run|show|list|close|quit|set|turn|adjust|"
    r"shut\s?down|power|reboot|restart|brightness|status|uptime|"
    r"terminal|app|apps)\b", re.I)

def _has_action_intent(text):
    return bool(_ACTION_CUE.search(text or ""))

HERE = os.path.dirname(os.path.abspath(__file__))

def _default_tools_path():
    """Find aura-tools.json across the layouts it ships in (installed vs repo).
    Returning a real path matters: a missing file made every /ask raise."""
    for c in (os.path.join(HERE, "config", "aura-tools.json"),
              os.path.join(HERE, "..", "config", "aura-tools.json"),
              "/opt/aura/config/aura-tools.json",
              "/usr/share/aurora/config/aura-tools.json",
              "/aurora/config/aura-tools.json"):
        if os.path.exists(c):
            return c
    return os.path.join(HERE, "config", "aura-tools.json")

TOOLS_PATH = os.environ.get("AURA_TOOLS", _default_tools_path())
LLAMA_URL = os.environ.get("AURA_LLM_URL", "http://127.0.0.1:8080/v1/chat/completions")
LLAMA_TIMEOUT = float(os.environ.get("AURA_LLM_TIMEOUT", "90"))

def load_tools(path=None):
    """Load the tool registry; return [] if it can't be read so /ask never 500s."""
    try:
        with open(path or TOOLS_PATH, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return []

def build_prompt(tools, user_text):
    lines = []
    for t in tools:
        args = ", ".join(f"{k} ({v})" for k, v in t["args"].items()) or "none"
        lines.append(f'- {t["name"]}: {t["description"]} args: {args}')
    system = (
        "You are Aura, the friendly on-device AI assistant built into AuroraOS, a "
        "Linux desktop. You run entirely on the user's own device — no cloud. "
        "Chat naturally and helpfully, and keep answers concise (1-3 sentences "
        "unless the user asks for more).\n"
        "Only when the user clearly asks you to perform a desktop action, reply with "
        "a single JSON object and nothing else, for example:\n"
        '{"reply": "Opening a terminal.", "tool_calls": [{"cmd": "open_terminal", "args": {}}]}\n'
        "Available actions:\n" + "\n".join(lines) + "\n"
        "Use only these actions with these args; never invent them. For ordinary "
        "conversation, questions, or explanations, just answer in plain text."
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
        if isinstance(obj, dict) and ("tool_calls" in obj or "reply" in obj):
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
    """Deterministic reply when the model is unavailable or produced garbage."""
    qs = (user_text or "").lower().strip()
    status = status or {}
    if any(qs == g or qs.startswith(g + " ") for g in ("hi", "hello", "hey", "yo", "hiya")):
        return "Hi! I'm Aura, your on-device assistant. Ask me anything, or tell me to open an app or check the system."
    if "battery" in qs:
        b = status.get("battery") or {}
        pct = b.get("percent")
        return (f"Battery is at {pct}% ({b.get('status','')})." if pct is not None
                else "No battery — running on AC power.")
    if "bright" in qs:
        return f"Brightness is {status.get('brightness') or 'not controllable on this device'}%."
    if any(k in qs for k in ("off", "shutdown", "power")):
        return "You can shut down or restart from the power controls, or just ask me to."
    return ("I'm still warming up — the on-device model is loading. In the meantime I "
            "can open apps, open a terminal, and report system status.")

def _reply_is_bad(reply):
    """True if the text is empty, leaked JSON, or an unfilled prompt placeholder
    (small models sometimes echo the template, e.g. '<short confirmation>')."""
    r = (reply or "").strip()
    if not r:
        return True
    if r.startswith("{") or r.startswith("["):
        return True
    if "<short confirmation>" in r or "<tool>" in r or "<reply>" in r:
        return True
    if r.startswith("<") and r.endswith(">") and len(r) < 60:
        return True
    return False

def call_llama(system, user):
    """POST to llama-server; return assistant content or None on any failure."""
    body = json.dumps({
        "messages": [{"role": "system", "content": system},
                     {"role": "user", "content": user}],
        "temperature": 0.2,
        "max_tokens": 128,
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
    calls = parsed["tool_calls"]
    if calls and not _has_action_intent(user_text):
        calls = []   # model invented an action for conversational input — ignore it
    actions, notes = route(calls, tools, executors)
    reply = parsed["reply"] or ""
    if notes:
        reply = (reply + " " + " ".join(notes)).strip()
    # Small models sometimes leak raw/partial JSON or echo the prompt template
    # (e.g. "<short confirmation>"). Never surface that — fall back to clean text.
    # If an action actually ran, keep the note text even if the model's prose was bad.
    if _reply_is_bad(reply):
        reply = " ".join(notes).strip() if notes else heuristic_fallback(user_text, status)
    return {"a": reply, "actions": actions}
