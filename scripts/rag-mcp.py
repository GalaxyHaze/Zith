#!/usr/bin/env python3
"""MCP stdio server for the local, daemon-free RAG search in Zith.

Exposes read-only tools:
  - rag_search: keyword + optional embedding search over code or docs
  - rag_show:   show one indexed chunk
  - rag_stats:  index summary

The server delegates to scripts/rag.py and never starts a network daemon.
Diagnostics go to stderr; stdout stays JSON-RPC only.

Environment:
  RAG_PROJECT_DIR   project root (default: repository root)
  RAG_INDEX_DIR     index directory (default: <root>/.rag-index)
  RAG_EMBEDDER      optional llama-embedding executable
  RAG_EMBEDDING_MODEL  optional embedding GGUF path
  RAG_TIMEOUT_SEC   subprocess timeout (default: 60)
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any


VERSION = "1.0.0"
SERVER_NAME = "rag-mcp"
SERVER_VERSION = "0.1.0"


def _default_root() -> Path:
    return Path(__file__).resolve().parent.parent


def _rag_script() -> Path:
    return Path(__file__).resolve().parent / "rag.py"


def _project_dir() -> Path:
    value = os.environ.get("RAG_PROJECT_DIR", "")
    if value:
        return Path(value).resolve()
    return _default_root()


def _index_dir() -> Path:
    env = os.environ.get("RAG_INDEX_DIR", "")
    if env:
        return Path(env).resolve()
    return _project_dir() / ".rag-index"


def _timeout() -> float:
    try:
        return float(os.environ.get("RAG_TIMEOUT_SEC", "60"))
    except ValueError:
        return 60.0


def _embedder_env() -> tuple[str, str] | None:
    embedder = os.environ.get("RAG_EMBEDDER", "")
    model = os.environ.get("RAG_EMBEDDING_MODEL", "")
    if embedder and model:
        return embedder, model
    return None


def _log(msg: str) -> None:
    print(f"[rag-mcp] {msg}", file=sys.stderr, flush=True)


def _send(msg: dict[str, Any]) -> None:
    print(json.dumps(msg, ensure_ascii=False, separators=(",", ":")), flush=True)


def _error(id_: str | int | None, code: int, message: str, data: Any = None) -> dict[str, Any]:
    err: dict[str, Any] = {"code": code, "message": message}
    if data is not None:
        err["data"] = data
    return {"jsonrpc": "2.0", "id": id_, "error": err}


def _result(id_: str | int | None, result: Any) -> dict[str, Any]:
    return {"jsonrpc": "2.0", "id": id_, "result": result}


def _run_rag(args: list[str]) -> dict[str, Any]:
    root = _project_dir()
    index_dir = _index_dir()
    cmd = [sys.executable, str(_rag_script())]
    if index_dir != root / ".rag-index":
        cmd += ["--index-dir", str(index_dir)]
    else:
        cmd += ["--index-dir", str(index_dir)]
    cmd += args
    env = os.environ.copy()
    env["RAG_PROJECT_DIR"] = str(root)
    env["RAG_INDEX_DIR"] = str(index_dir)

    _log(f"run: {cmd!r} cwd={root} timeout={_timeout()}s")
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            cwd=root,
            timeout=_timeout(),
            env=env,
        )
    except FileNotFoundError as exc:
        return {"isError": True, "content": [{"type": "text", "text": json.dumps({"error": str(exc)})}]}
    except subprocess.TimeoutExpired as exc:
        return {
            "isError": True,
            "content": [{"type": "text", "text": json.dumps({"error": "rag.py timed out", "stderr": exc.stderr or ""})}],
        }

    stderr = proc.stderr.strip()
    stdout = proc.stdout.strip()
    if proc.returncode == 0 and stdout:
        try:
            parsed = json.loads(stdout)
            return {"content": [{"type": "text", "text": json.dumps(parsed, ensure_ascii=False)}]}
        except json.JSONDecodeError:
            return {"content": [{"type": "text", "text": stdout}]}
    return {
        "isError": True,
        "content": [
            {
                "type": "text",
                "text": json.dumps(
                    {
                        "error": stdout or "rag.py failed",
                        "exit_code": proc.returncode,
                        "stderr": stderr[:2000],
                    },
                    ensure_ascii=False,
                ),
            }
        ],
    }


def _validate_query(value: Any) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValueError("query must be a non-empty string")
    if len(value) > 2000:
        raise ValueError("query too long (max 2000 characters)")
    return value.strip()


def _validate_top_k(value: Any) -> int:
    if value is None:
        return 10
    if not isinstance(value, (int, float)):
        raise ValueError("top_k must be an integer")
    k = int(value)
    if k < 1 or k > 50:
        raise ValueError("top_k must be between 1 and 50")
    return k


def _validate_kind(value: Any) -> str:
    if value is None:
        return "code"
    if value not in ("code", "docs"):
        raise ValueError("kind must be 'code' or 'docs'")
    return value


def tool_rag_search(args: dict[str, Any]) -> dict[str, Any]:
    query = _validate_query(args.get("query"))
    top_k = _validate_top_k(args.get("top_k"))
    kind = _validate_kind(args.get("kind"))
    cmd = ["search", query, "--kind", kind, "--top-k", str(top_k), "--json"]
    embed = _embedder_env()
    if embed:
        cmd += ["--embedder", embed[0], "--model", embed[1]]
    return _run_rag(cmd)


def tool_rag_show(args: dict[str, Any]) -> dict[str, Any]:
    chunk_id = args.get("chunk_id")
    kind = _validate_kind(args.get("kind"))
    if not isinstance(chunk_id, str) or not chunk_id.strip():
        raise ValueError("chunk_id must be a non-empty string")
    if len(chunk_id) > 64:
        raise ValueError("chunk_id too long")
    cmd = ["show", chunk_id, "--kind", kind]
    return _run_rag(cmd)


def tool_rag_stats() -> dict[str, Any]:
    return _run_rag(["stats"])


TOOLS = {
    "rag_search": {
        "name": "rag_search",
        "description": (
            "Local keyword + optional LLM-reranked search over the Zith workspace. "
            "kind='code' searches C++/headers/Zith sources; kind='docs' searches "
            "markdown docs and memory notes. Returns file/line-anchored chunks."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "Natural language or symbol search query."},
                "kind": {
                    "type": "string",
                    "enum": ["code", "docs"],
                    "description": "Corpus to search (default code).",
                },
                "top_k": {"type": "integer", "description": "Maximum results, default 10, max 50."},
            },
            "required": ["query"],
        },
    },
    "rag_show": {
        "name": "rag_show",
        "description": "Show the contents of one indexed chunk by chunk_id or exact term.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "chunk_id": {"type": "string", "description": "chunk_id from rag_search."},
                "kind": {
                    "type": "string",
                    "enum": ["code", "docs"],
                    "description": "Corpus (default code).",
                },
            },
            "required": ["chunk_id"],
        },
    },
    "rag_stats": {
        "name": "rag_stats",
        "description": "Show RAG index status and chunk counts.",
        "inputSchema": {"type": "object", "properties": {}},
    },
}


def _dispatch(request: dict[str, Any]) -> dict[str, Any] | None:
    method = request.get("method", "")
    id_ = request.get("id")
    params = request.get("params", {}) or {}

    if method == "initialize":
        return _result(
            id_,
            {
                "protocolVersion": VERSION,
                "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
                "capabilities": {"tools": {}},
            },
        )
    if method == "notifications/initialized":
        return None
    if method == "ping":
        return _result(id_, {})
    if method == "tools/list":
        return _result(id_, {"tools": list(TOOLS.values())})
    if method == "tools/call":
        name = params.get("name", "")
        if name not in TOOLS:
            return _error(id_, -32601, f"Unknown tool: {name}")
        try:
            if name == "rag_search":
                result = tool_rag_search(params.get("arguments", {}))
            elif name == "rag_show":
                result = tool_rag_show(params.get("arguments", {}))
            else:
                result = tool_rag_stats()
        except ValueError as exc:
            result = {
                "content": [{"type": "text", "text": json.dumps({"error": str(exc)})}],
                "isError": True,
            }
        return _result(id_, result)
    return _error(id_, -32601, f"Method not found: {method}")


def main() -> None:
    _log(f"starting {SERVER_NAME} v{SERVER_VERSION}")
    _log(f"project dir: {_project_dir()}")
    _log(f"index dir: {_index_dir()}")
    env = _embedder_env()
    if env:
        _log(f"embedder: {env[0]}")
        _log(f"embedding model: {env[1]}")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except json.JSONDecodeError as exc:
            _log(f"invalid JSON on stdin: {exc}")
            continue
        try:
            response = _dispatch(request)
        except Exception as exc:
            _log(f"unhandled exception: {exc}")
            response = _error(request.get("id"), -32603, f"Internal error: {exc}")
        if response is not None:
            _send(response)


if __name__ == "__main__":
    main()
