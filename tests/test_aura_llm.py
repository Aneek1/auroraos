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
