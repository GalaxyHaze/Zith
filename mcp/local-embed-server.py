#!/usr/bin/env python3
"""Minimal OpenAI-compatible /embeddings endpoint backed by memsearch ONNX runtime.

The opencode-rag plugin only supports HTTP embedding providers via its
OpenAI-compatible client.  This small server exposes the already-cached
memsearch ONNX model as `POST /v1/embeddings` so indexing works in this
workspace without Ollama running.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    from memsearch.embeddings.onnx import OnnxEmbedding
except ImportError:
    sys.path.insert(0, "/home/diogo/.local/share/uv/tools/memsearch/lib/python3.14/site-packages")
    from memsearch.embeddings.onnx import OnnxEmbedding


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--listen", default="127.0.0.1")
    parser.add_argument("--model", default="gpahal/bge-m3-onnx-int8")
    args = parser.parse_args()

    embedder = OnnxEmbedding(model=args.model)
    # Warm the session and probe the dimension once before accepting requests.
    asyncio.run(embedder.embed(["warmup"]))

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt: str, *values: object) -> None:
            sys.stderr.write(f"[local-embed] {self.command} {self.path}\n")

        def do_GET(self) -> None:
            if self.path == "/v1/models":
                self._send_json(
                    200,
                    {
                        "object": "list",
                        "data": [{"id": args.model, "object": "model", "owned_by": "local"}],
                    },
                )
                return
            self._send_json(404, {"error": {"message": "not found", "type": "not_found"}})

        def do_POST(self) -> None:
            if self.path != "/v1/embeddings":
                self._send_json(404, {"error": {"message": "not found", "type": "not_found"}})
                return

            try:
                n = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(n))
                texts = body.get("input")
                model = body.get("model", args.model)
                if not isinstance(texts, str) and not isinstance(texts, list):
                    raise ValueError("input must be a string or array of strings")
                if isinstance(texts, str):
                    texts = [texts]
                if not all(isinstance(t, str) for t in texts):
                    raise ValueError("input must be an array of strings")

                embeddings = asyncio.run(embedder.embed(texts))
                # The plugin expects OpenAI's response shape with data[].embedding.
                data = [
                    {
                        "object": "embedding",
                        "embedding": [float(v) for v in vector],
                        "index": idx,
                    }
                    for idx, vector in enumerate(embeddings)
                ]
                self._send_json(
                    200,
                    {
                        "object": "list",
                        "model": model,
                        "data": data,
                        "usage": {
                            "prompt_tokens": sum(len(t.split()) for t in texts),
                            "total_tokens": sum(len(t.split()) for t in texts),
                        },
                    },
                )
            except Exception as exc:
                self._send_json(
                    400,
                    {"error": {"message": str(exc), "type": "invalid_request_error"}},
                )

        def _send_json(self, status: int, payload: dict) -> None:
            encoded = json.dumps(payload).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)

    httpd = ThreadingHTTPServer((args.listen, args.port), Handler)
    print(json.dumps({"host": args.listen, "port": httpd.server_address[1]}), flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
