#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PYTHON_BIN="${EMBED_PYTHON:-/home/diogo/.local/share/uv/tools/memsearch/bin/python}"
SERVER_PORT_FILE="$(mktemp)"
LOCAL_CONFIG="$ROOT/.opencode/openCodeRag-cli.local.json"
SERVER_PID=""

cleanup() {
  if [[ -n "$SERVER_PID" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -f "$SERVER_PORT_FILE" "$LOCAL_CONFIG"
}
trap cleanup EXIT

"$PYTHON_BIN" "$ROOT/mcp/local-embed-server.py" --listen 127.0.0.1 --port 0 \
  > "$SERVER_PORT_FILE" 2>/dev/null &
SERVER_PID=$!

for _ in $(seq 1 120); do
  if [[ -s "$SERVER_PORT_FILE" ]]; then
    break
  fi
  sleep 1
done

if [[ ! -s "$SERVER_PORT_FILE" ]]; then
  echo "local embedding server did not start" >&2
  exit 1
fi

PORT="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' "$SERVER_PORT_FILE")"

python3 - "$ROOT/opencode-rag.json" "$LOCAL_CONFIG" "$PORT" <<'PY'
import json, sys
source, target, port = sys.argv[1:]
with open(source, encoding="utf-8") as f:
    cfg = json.load(f)
cfg["embedding"] = {
    "provider": cfg.get("localEmbedding", {}).get("provider", "openai"),
    "baseUrl": f"http://127.0.0.1:{port}/v1",
    "model": cfg.get("localEmbedding", {}).get("model", "gpahal/bge-m3-onnx-int8"),
    "apiKey": cfg.get("localEmbedding", {}).get("apiKey", "local"),
    "timeoutMs": 600000,
    "vectorDimension": cfg.get("localEmbedding", {}).get("vectorDimension", 1024),
}
with open(target, "w", encoding="utf-8") as f:
    json.dump(cfg, f, indent=2)
PY

opencode-rag index -c "$LOCAL_CONFIG" -y
