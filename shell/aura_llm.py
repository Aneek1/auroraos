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
