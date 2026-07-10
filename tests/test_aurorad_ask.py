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
