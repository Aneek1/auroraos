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
