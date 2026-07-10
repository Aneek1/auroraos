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

def test_fallback_reports_battery():
    r = aura_llm.heuristic_fallback("what's my battery",
                                    status={"battery": {"percent": 88, "status": "Discharging"}})
    assert "88%" in r

def test_fallback_default_message():
    r = aura_llm.heuristic_fallback("tell me a joke", status={})
    assert "battery" in r.lower() or "status" in r.lower()

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
