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
