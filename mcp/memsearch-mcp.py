#!/usr/bin/env python3
"""memsearch MCP server — stdio JSON-RPC adapter for ByteAsk.

Exposes read-only semantic search and context expansion through the MCP
protocol, delegating to the installed `memsearch` CLI.  Write diagnostics
to stderr; never emit unsolicited output to stdout.

Environment
    MEMSEARCH_BIN           path to the memsearch executable (default: `memsearch` from PATH)
    MEMSEARCH_PROJECT_DIR   working directory for subprocess invocations (default: cwd)
    MEMSEARCH_TIMEOUT_SEC   subprocess timeout in seconds (default: 30)
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import threading
import time
from typing import Any

# ── protocol constants ──────────────────────────────────────────────────────

VERSION = "2024-11-05"
SERVER_NAME = "memsearch-mcp"
SERVER_VERSION = "0.1.0"

# ── helpers ─────────────────────────────────────────────────────────────────

def _log(msg: str) -> None:
    """Write a diagnostic line to stderr."""
    print(f"[memsearch-mcp] {msg}", file=sys.stderr, flush=True)


def _send(msg: dict[str, Any]) -> None:
    """Write a single JSON-RPC message to stdout followed by a newline."""
    line = json.dumps(msg, ensure_ascii=False, separators=(",", ":"))
    sys.stdout.write(line + "\n")
    sys.stdout.flush()


def _error(id_: str | int | None, code: int, message: str, data: Any = None) -> dict[str, Any]:
    """Build a JSON-RPC error response."""
    err: dict[str, Any] = {"code": code, "message": message}
    if data is not None:
        err["data"] = data
    resp: dict[str, Any] = {"jsonrpc": "2.0", "id": id_, "error": err}
    return resp


def _result(id_: str | int | None, result: Any) -> dict[str, Any]:
    """Build a JSON-RPC success response."""
    return {"jsonrpc": "2.0", "id": id_, "result": result}


def _resolve_bin() -> str:
    """Return the memsearch executable path from env or PATH."""
    env = os.environ.get("MEMSEARCH_BIN", "")
    if env:
        return env
    found = shutil.which("memsearch")
    if found:
        return found
    return "memsearch"  # let subprocess report the failure


def _project_dir() -> str:
    return os.environ.get("MEMSEARCH_PROJECT_DIR", os.getcwd())


def _timeout() -> float:
    try:
        return float(os.environ.get("MEMSEARCH_TIMEOUT_SEC", "30"))
    except ValueError:
        return 30.0


# ── subprocess runner ───────────────────────────────────────────────────────

class SubprocessError(Exception):
    """A controlled failure from the memsearch subprocess."""

    def __init__(self, message: str, exit_code: int | None = None, stderr: str = "") -> None:
        super().__init__(message)
        self.exit_code = exit_code
        self.stderr = stderr


def _run(args: list[str]) -> dict[str, Any]:
    """Run memsearch with *args*, return parsed JSON stdout or raise SubprocessError."""
    bin_path = _resolve_bin()
    cmd = [bin_path] + args
    cwd = _project_dir()
    timeout = _timeout()

    _log(f"exec: {cmd!r}  cwd={cwd!r}  timeout={timeout}s")
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            cwd=cwd,
            timeout=timeout,
        )
    except FileNotFoundError:
        raise SubprocessError(
            f"memsearch executable not found: {bin_path}. "
            "Install memsearch or set MEMSEARCH_BIN."
        ) from None
    except subprocess.TimeoutExpired:
        raise SubprocessError(
            f"memsearch timed out after {timeout}s",
            stderr="",
        ) from None

    stderr_text = proc.stderr.strip()
    if proc.returncode != 0:
        raise SubprocessError(
            f"memsearch exited with code {proc.returncode}",
            exit_code=proc.returncode,
            stderr=stderr_text,
        )

    stdout_text = proc.stdout.strip()
    if not stdout_text:
        raise SubprocessError("memsearch returned empty output", stderr=stderr_text)

    try:
        return json.loads(stdout_text)  # type: ignore[no-any-return]
    except json.JSONDecodeError:
        raise SubprocessError(
            "memsearch returned non-JSON output",
            stderr=stderr_text,
        ) from None


# ── validation ──────────────────────────────────────────────────────────────

def _validate_query(query: Any) -> str:
    if not isinstance(query, str) or not query.strip():
        raise ValueError("query must be a non-empty string")
    if len(query) > 2000:
        raise ValueError("query too long (max 2000 characters)")
    return query.strip()


def _validate_top_k(top_k: Any) -> int:
    if top_k is None:
        return 5
    if not isinstance(top_k, (int, float)):
        raise ValueError("top_k must be an integer")
    k = int(top_k)
    if k < 1 or k > 50:
        raise ValueError("top_k must be between 1 and 50")
    return k


def _validate_chunk_hash(chunk_hash: Any) -> str:
    if not isinstance(chunk_hash, str) or not chunk_hash.strip():
        raise ValueError("chunk_hash must be a non-empty string")
    if len(chunk_hash) > 256:
        raise ValueError("chunk_hash too long")
    # Basic sanity: should look like a hex hash
    sanitized = chunk_hash.strip()
    if not all(c in "0123456789abcdefABCDEF-" for c in sanitized):
        raise ValueError("chunk_hash contains invalid characters")
    return sanitized


# ── tool implementations ────────────────────────────────────────────────────

def tool_memsearch_search(arguments: dict[str, Any]) -> dict[str, Any]:
    """Execute a semantic memory search via memsearch CLI."""
    query = _validate_query(arguments.get("query"))
    top_k = _validate_top_k(arguments.get("top_k"))
    source_prefix = arguments.get("source_prefix")

    args = ["search", query, "--json-output", "--top-k", str(top_k)]
    if source_prefix and isinstance(source_prefix, str):
        args.extend(["--source-prefix", source_prefix])

    try:
        result = _run(args)
    except SubprocessError as exc:
        return {
            "content": [
                {
                    "type": "text",
                    "text": json.dumps(
                        {
                            "error": str(exc),
                            "exit_code": exc.exit_code,
                            "stderr": exc.stderr[:2000] if exc.stderr else None,
                        },
                        ensure_ascii=False,
                    ),
                }
            ],
            "isError": True,
        }

    # result is already a list from memsearch --json-output
    return {
        "content": [
            {
                "type": "text",
                "text": json.dumps(result, ensure_ascii=False),
            }
        ]
    }


def tool_memsearch_expand(arguments: dict[str, Any]) -> dict[str, Any]:
    """Expand a chunk to its full section context via memsearch CLI."""
    chunk_hash = _validate_chunk_hash(arguments.get("chunk_hash"))
    lines = arguments.get("lines")

    args = ["expand", chunk_hash, "--json-output"]
    if lines is not None:
        if not isinstance(lines, (int, float)):
            return {
                "content": [{"type": "text", "text": json.dumps({"error": "lines must be an integer"})}],
                "isError": True,
            }
        l = int(lines)
        if l < 1 or l > 500:
            return {
                "content": [{"type": "text", "text": json.dumps({"error": "lines must be between 1 and 500"})}],
                "isError": True,
            }
        args.extend(["--lines", str(l)])

    try:
        result = _run(args)
    except SubprocessError as exc:
        return {
            "content": [
                {
                    "type": "text",
                    "text": json.dumps(
                        {
                            "error": str(exc),
                            "exit_code": exc.exit_code,
                            "stderr": exc.stderr[:2000] if exc.stderr else None,
                        },
                        ensure_ascii=False,
                    ),
                }
            ],
            "isError": True,
        }

    return {
        "content": [
            {
                "type": "text",
                "text": json.dumps(result, ensure_ascii=False),
            }
        ]
    }


# ── MCP message dispatch ────────────────────────────────────────────────────

TOOLS = {
    "memsearch_search": {
        "name": "memsearch_search",
        "description": (
            "Semantic search across indexed project memory (markdown knowledge base). "
            "Returns ranked chunks with content, source, heading, score, and chunk_hash. "
            "Use memsearch_expand to retrieve full section context for a result."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {
                    "type": "string",
                    "description": "Natural-language search query.",
                },
                "top_k": {
                    "type": "integer",
                    "description": "Maximum number of results to return (default 5, max 50).",
                },
                "source_prefix": {
                    "type": "string",
                    "description": "Optional path prefix to scope results to a subdirectory.",
                },
            },
            "required": ["query"],
        },
    },
    "memsearch_expand": {
        "name": "memsearch_expand",
        "description": (
            "Expand a chunk (identified by chunk_hash from a search result) to its full "
            "markdown section context. Returns the complete heading section or N lines "
            "of surrounding context."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "chunk_hash": {
                    "type": "string",
                    "description": "The chunk_hash from a memsearch_search result.",
                },
                "lines": {
                    "type": "integer",
                    "description": "Show N lines before/after the chunk instead of full section (1-500).",
                },
            },
            "required": ["chunk_hash"],
        },
    },
}


def _dispatch(request: dict[str, Any]) -> dict[str, Any] | None:
    """Route an incoming JSON-RPC request; return a response or None (for notifications)."""
    method = request.get("method", "")
    id_ = request.get("id")
    params = request.get("params", {})

    if method == "initialize":
        return _result(
            id_,
            {
                "protocolVersion": VERSION,
                "serverInfo": {
                    "name": SERVER_NAME,
                    "version": SERVER_VERSION,
                },
                "capabilities": {
                    "tools": {},
                },
            },
        )

    if method == "notifications/initialized":
        return None  # notification, no response

    if method == "tools/list":
        return _result(id_, {"tools": list(TOOLS.values())})

    if method == "tools/call":
        tool_name = params.get("name", "")
        tool_args = params.get("arguments", {})
        if tool_name not in TOOLS:
            return _error(id_, -32601, f"Unknown tool: {tool_name}")
        if tool_name == "memsearch_search":
            try:
                tool_result = tool_memsearch_search(tool_args)
            except ValueError as exc:
                tool_result = {
                    "content": [{"type": "text", "text": json.dumps({"error": str(exc)})}],
                    "isError": True,
                }
            return _result(id_, tool_result)
        if tool_name == "memsearch_expand":
            try:
                tool_result = tool_memsearch_expand(tool_args)
            except ValueError as exc:
                tool_result = {
                    "content": [{"type": "text", "text": json.dumps({"error": str(exc)})}],
                    "isError": True,
                }
            return _result(id_, tool_result)

    if method == "ping":
        return _result(id_, {})

    return _error(id_, -32601, f"Method not found: {method}")


# ── main loop ───────────────────────────────────────────────────────────────

def main() -> None:
    """Read JSON-RPC lines from stdin, dispatch, write responses to stdout."""
    _log(f"starting {SERVER_NAME} v{SERVER_VERSION}")
    _log(f"memsearch bin: {_resolve_bin()}")
    _log(f"project dir:   {_project_dir()}")
    _log(f"timeout:       {_timeout()}s")

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
