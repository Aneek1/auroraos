#!/bin/bash
# Live intent smoke: run the fixtures against a running llama-server.
# Skips (exit 0) if no server is reachable, so CI without a model still passes.
set -u
URL="${AURA_LLM_URL:-http://127.0.0.1:8080/v1/chat/completions}"
curl -s -o /dev/null --max-time 2 "$URL" || { echo "SKIP: no llama-server at $URL"; exit 0; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
pass=0; total=0
while IFS= read -r line; do
  [ -z "$line" ] && continue
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
