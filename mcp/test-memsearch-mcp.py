#!/usr/bin/env python3
"""Tests for memsearch-mcp.py using a fake memsearch executable.

Run:
    python3 scripts/test-memsearch-mcp.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import textwrap
import time
from pathlib import Path

# ── helpers ─────────────────────────────────────────────────────────────────

SCRIPT = Path(__file__).resolve().parent / "memsearch-mcp.py"
PYTHON = sys.executable

passed = 0
failed = 0


def check(name: str, condition: bool, detail: str = "") -> None:
    global passed, failed
    if condition:
        passed += 1
        print(f"  PASS  {name}")
    else:
        failed += 1
        print(f"  FAIL  {name}" + (f"  — {detail}" if detail else ""))


def make_fake_memsearch(tmpdir: str, responses: dict[str, str]) -> str:
    """Create a fake memsearch script that returns canned JSON for each subcommand."""
    script_path = os.path.join(tmpdir, "memsearch")
    lines = ["#!/usr/bin/env python3", "import sys, json", ""]
    lines.append("args = sys.argv[1:]")
    lines.append("key = ' '.join(args)")
    lines.append("resp = " + json.dumps(responses))
    lines.append("if key in resp:")
    lines.append("    print(resp[key])")
    lines.append("    sys.exit(0)")
    lines.append("else:")
    lines.append("    print(f'UNKNOWN: {key}', file=sys.stderr)")
    lines.append("    sys.exit(2)")
    with open(script_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    os.chmod(script_path, 0o755)
    return script_path


def make_fake_memsearch_error(tmpdir: str, exit_code: int = 1, stderr: str = "backend error") -> str:
    """Create a fake memsearch that always exits with an error."""
    script_path = os.path.join(tmpdir, "memsearch_err")
    lines = [
        "#!/usr/bin/env python3",
        "import sys",
        f"print({json.dumps(stderr)}, file=sys.stderr)",
        f"sys.exit({exit_code})",
    ]
    with open(script_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    os.chmod(script_path, 0o755)
    return script_path


def make_fake_memsearch_empty(tmpdir: str) -> str:
    """Create a fake memsearch that exits 0 with empty stdout."""
    script_path = os.path.join(tmpdir, "memsearch_empty")
    lines = ["#!/usr/bin/env python3", "import sys", "sys.exit(0)"]
    with open(script_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    os.chmod(script_path, 0o755)
    return script_path


def make_fake_memsearch_non_json(tmpdir: str) -> str:
    """Create a fake memsearch that exits 0 with non-JSON stdout."""
    script_path = os.path.join(tmpdir, "memsearch_nonjson")
    lines = ["#!/usr/bin/env python3", "import sys", "print('not json')", "sys.exit(0)"]
    with open(script_path, "w") as f:
        f.write("\n".join(lines) + "\n")  # fixed: added missing import
    os.chmod(script_path, 0o755)
    return script_path


def send_request(proc: subprocess.Popen, req: dict) -> dict:
    """Send a JSON-RPC request and return the parsed response."""
    line = json.dumps(req, ensure_ascii=False)
    proc.stdin.write(line + "\n")
    proc.stdin.flush()
    # Read response
    output = proc.stdout.readline()
    if not output:
        raise RuntimeError(f"No response for request {req.get('method')}")
    return json.loads(output)


def start_server(env: dict[str, str] | None = None) -> subprocess.Popen:
    """Start the MCP server process."""
    full_env = os.environ.copy()
    if env:
        full_env.update(env)
    proc = subprocess.Popen(
        [PYTHON, str(SCRIPT)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=full_env,
    )
    # Give the server a moment to start
    time.sleep(0.1)
    return proc


# ── test cases ──────────────────────────────────────────────────────────────

def test_initialize() -> None:
    """MCP initialize returns server info and capabilities."""
    print("\n[test_initialize]")
    proc = start_server()
    try:
        resp = send_request(proc, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
        check("jsonrpc field", resp.get("jsonrpc") == "2.0")
        check("id matches", resp.get("id") == 1)
        result = resp.get("result", {})
        check("protocol version", result.get("protocolVersion") == "1.0.0")
        check("server name", result["serverInfo"]["name"] == "memsearch-mcp")
        check("tools capability", "tools" in result.get("capabilities", {}))
    finally:
        proc.terminate()
        proc.wait(timeout=5)


def test_tools_list() -> None:
    """tools/list returns exactly two tools."""
    print("\n[test_tools_list]")
    proc = start_server()
    try:
        send_request(proc, {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}})
        resp = send_request(proc, {"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}})
        tools = resp["result"]["tools"]
        names = {t["name"] for t in tools}
        check("exactly memsearch_search", "memsearch_search" in names)
        check("exactly memsearch_expand", "memsearch_expand" in names)
        check("only two tools", len(tools) == 2)
    finally:
        proc.terminate()
        proc.wait(timeout=5)


def test_search_success() -> None:
    """memsearch_search returns JSON results from a healthy backend."""
    print("\n[test_search_success]")
    with tempfile.TemporaryDirectory() as tmpdir:
        fake_bin = make_fake_memsearch(
            tmpdir,
            {
                "search test query --json-output --top-k 5": json.dumps(
                    [
                        {
                            "content": "something relevant",
                            "source": "/docs/notes.md",
                            "heading": "Architecture",
                            "score": 0.95,
                            "chunk_hash": "abc123",
                        }
                    ]
                )
            },
        )
        proc = start_server({"MEMSEARCH_BIN": fake_bin, "MEMSEARCH_PROJECT_DIR": tmpdir})
        try:
            send_request(proc, {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}})
            resp = send_request(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "tools/call",
                    "params": {"name": "memsearch_search", "arguments": {"query": "test query"}},
                },
            )
            result = resp["result"]
            check("not isError", not result.get("isError"))
            text = result["content"][0]["text"]
            parsed = json.loads(text)
            check("result is a list", isinstance(parsed, list))
            check("has one result", len(parsed) == 1)
            check("correct chunk_hash", parsed[0]["chunk_hash"] == "abc123")
            check("correct score", parsed[0]["score"] == 0.95)
        finally:
            proc.terminate()
            proc.wait(timeout=5)


def test_search_with_source_prefix() -> None:
    """memsearch_search passes --source-prefix."""
    print("\n[test_search_with_source_prefix]")
    with tempfile.TemporaryDirectory() as tmpdir:
        fake_bin = make_fake_memsearch(
            tmpdir,
            {
                "search memory --json-output --top-k 3 --source-prefix /docs": json.dumps([])
            },
        )
        proc = start_server({"MEMSEARCH_BIN": fake_bin, "MEMSEARCH_PROJECT_DIR": tmpdir})
        try:
            send_request(proc, {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}})
            resp = send_request(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 2,
                    "method": "tools/call",
                    "params": {
                        "name": "memsearch_search",
                        "arguments": {"query": "memory", "top_k": 3, "source_prefix": "/docs"},
                    },
                },
            )
            check("request succeeded (empty result)", not resp["result"].get("isError"))
        finally:
            proc.terminate()
            proc.wait(timeout=5)


def test_expand_success() -> None:
    """memsearch_expand returns expanded context."""
    print("\n[test_expand_success]")
    with tempfile.TemporaryDirectory() as tmpdir:
        fake_bin = make_fake_memsearch(
            tmpdir,
            {
                "expand abc123 --json-output": json.dumps(
                    {
                        "chunk_hash": "abc123",
                        "source": "/docs/notes.md",
                        "heading": "Architecture",
                        "start_line": 10,
                        "end_line": 40,
                        "content": "# Architecture\n\nFull section text...",
                    }
                )
            },
        )
        proc = start_server({"MEMSEARCH_BIN": fake_bin, "MEMSEARCH_PROJECT_DIR": tmpdir})
        try:
            send_request(proc, {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}})
            resp = send_request(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "tools/call",
                    "params": {"name": "memsearch_expand", "arguments": {"chunk_hash": "abc123"}},
                },
            )
            result = resp["result"]
            check("not isError", not result.get("isError"))
            parsed = json.loads(result["content"][0]["text"])
            check("correct hash", parsed["chunk_hash"] == "abc123")
            check("has content", "content" in parsed)
        finally:
            proc.terminate()
            proc.wait(timeout=5)


def test_expand_with_lines() -> None:
    """memsearch_expand passes --lines."""
    print("\n[test_expand_with_lines]")
    with tempfile.TemporaryDirectory() as tmpdir:
        fake_bin = make_fake_memsearch(
            tmpdir,
            {
                "expand def456 --json-output --lines 20": json.dumps({"chunk_hash": "def456", "content": "..."})
            },
        )
        proc = start_server({"MEMSEARCH_BIN": fake_bin, "MEMSEARCH_PROJECT_DIR": tmpdir})
        try:
            send_request(proc, {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}})
            resp = send_request(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 4,
                    "method": "tools/call",
                    "params": {
                        "name": "memsearch_expand",
                        "arguments": {"chunk_hash": "def456", "lines": 20},
                    },
                },
            )
            check("not isError", not resp["result"].get("isError"))
        finally:
            proc.terminate()
            proc.wait(timeout=5)


def test_missing_executable() -> None:
    """Missing memsearch produces a structured error."""
    print("\n[test_missing_executable]")
    proc = start_server({"MEMSEARCH_BIN": "/nonexistent/memsearch_xyz", "MEMSEARCH_PROJECT_DIR": "/tmp"})
    try:
        send_request(proc, {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}})
        resp = send_request(
            proc,
            {
                "jsonrpc": "2.0",
                "id": 5,
                "method": "tools/call",
                "params": {"name": "memsearch_search", "arguments": {"query": "test"}},
            },
        )
        result = resp["result"]
        check("isError set", result.get("isError") is True)
        text = result["content"][0]["text"]
        err_obj = json.loads(text)
        check("error message references missing bin", "not found" in err_obj.get("error", "").lower())
    finally:
        proc.terminate()
        proc.wait(timeout=5)


def test_backend_error() -> None:
    """Non-zero exit from memsearch produces a structured error."""
    print("\n[test_backend_error]")
    with tempfile.TemporaryDirectory() as tmpdir:
        fake_bin = make_fake_memsearch_error(tmpdir, exit_code=1, stderr="milvus connection failed")
        proc = start_server({"MEMSEARCH_BIN": fake_bin, "MEMSEARCH_PROJECT_DIR": tmpdir})
        try:
            send_request(proc, {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}})
            resp = send_request(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 6,
                    "method": "tools/call",
                    "params": {"name": "memsearch_search", "arguments": {"query": "test"}},
                },
            )
            result = resp["result"]
            check("isError set", result.get("isError") is True)
            text = result["content"][0]["text"]
            err_obj = json.loads(text)
            check("exit code reported", err_obj.get("exit_code") == 1)
            check("stderr captured", "milvus" in err_obj.get("stderr", ""))
        finally:
            proc.terminate()
            proc.wait(timeout=5)


def test_empty_query_rejected() -> None:
    """Empty query is rejected before subprocess invocation."""
    print("\n[test_empty_query_rejected]")
    proc = start_server()
    try:
        send_request(proc, {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}})
        resp = send_request(
            proc,
            {
                "jsonrpc": "2.0",
                "id": 7,
                "method": "tools/call",
                "params": {"name": "memsearch_search", "arguments": {"query": "   "}},
            },
        )
        result = resp["result"]
        check("isError set", result.get("isError") is True)
        text = result["content"][0]["text"]
        err_obj = json.loads(text)
        check("error mentions query", "query" in err_obj.get("error", "").lower())
    finally:
        proc.terminate()
        proc.wait(timeout=5)


def test_invalid_top_k_rejected() -> None:
    """Invalid top_k is rejected."""
    print("\n[test_invalid_top_k_rejected]")
    proc = start_server()
    try:
        send_request(proc, {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}})
        resp = send_request(
            proc,
            {
                "jsonrpc": "2.0",
                "id": 8,
                "method": "tools/call",
                "params": {"name": "memsearch_search", "arguments": {"query": "x", "top_k": 999}},
            },
        )
        text = resp["result"]["content"][0]["text"]
        err_obj = json.loads(text)
        check("top_k rejected", "top_k" in err_obj.get("error", "").lower())
    finally:
        proc.terminate()
        proc.wait(timeout=5)


def test_empty_chunk_hash_rejected() -> None:
    """Empty chunk_hash is rejected."""
    print("\n[test_empty_chunk_hash_rejected]")
    proc = start_server()
    try:
        send_request(proc, {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}})
        resp = send_request(
            proc,
            {
                "jsonrpc": "2.0",
                "id": 9,
                "method": "tools/call",
                "params": {"name": "memsearch_expand", "arguments": {"chunk_hash": ""}},
            },
        )
        text = resp["result"]["content"][0]["text"]
        err_obj = json.loads(text)
        check("chunk_hash rejected", "chunk_hash" in err_obj.get("error", "").lower())
    finally:
        proc.terminate()
        proc.wait(timeout=5)


def test_unknown_tool() -> None:
    """Unknown tool name produces a protocol error."""
    print("\n[test_unknown_tool]")
    proc = start_server()
    try:
        send_request(proc, {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}})
        resp = send_request(
            proc,
            {
                "jsonrpc": "2.0",
                "id": 10,
                "method": "tools/call",
                "params": {"name": "nonexistent_tool", "arguments": {}},
            },
        )
        check("has error", "error" in resp)
        check("correct error code", resp["error"]["code"] == -32601)
    finally:
        proc.terminate()
        proc.wait(timeout=5)


def test_backend_empty_output() -> None:
    """Exit 0 with empty stdout is a controlled error."""
    print("\n[test_backend_empty_output]")
    with tempfile.TemporaryDirectory() as tmpdir:
        fake_bin = make_fake_memsearch_empty(tmpdir)
        proc = start_server({"MEMSEARCH_BIN": fake_bin, "MEMSEARCH_PROJECT_DIR": tmpdir})
        try:
            send_request(proc, {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}})
            resp = send_request(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 11,
                    "method": "tools/call",
                    "params": {"name": "memsearch_search", "arguments": {"query": "x"}},
                },
            )
            result = resp["result"]
            check("isError set on empty output", result.get("isError") is True)
        finally:
            proc.terminate()
            proc.wait(timeout=5)


def test_backend_non_json_output() -> None:
    """Exit 0 with non-JSON stdout is a controlled error."""
    print("\n[test_backend_non_json_output]")
    with tempfile.TemporaryDirectory() as tmpdir:
        fake_bin = make_fake_memsearch_non_json(tmpdir)
        proc = start_server({"MEMSEARCH_BIN": fake_bin, "MEMSEARCH_PROJECT_DIR": tmpdir})
        try:
            send_request(proc, {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}})
            resp = send_request(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 12,
                    "method": "tools/call",
                    "params": {"name": "memsearch_search", "arguments": {"query": "x"}},
                },
            )
            result = resp["result"]
            check("isError set on non-JSON", result.get("isError") is True)
        finally:
            proc.terminate()
            proc.wait(timeout=5)


def test_project_dir_is_passed() -> None:
    """Subprocess working directory is the configured project dir."""
    print("\n[test_project_dir_is_passed]")
    with tempfile.TemporaryDirectory() as tmpdir:
        # Fake script that echoes its cwd
        script_path = os.path.join(tmpdir, "memsearch")
        with open(script_path, "w") as f:
            f.write(
                textwrap.dedent("""\
                #!/usr/bin/env python3
                import json, os, sys
                if sys.argv[1] == "search":
                    print(json.dumps([{"cwd": os.getcwd()}]))
                    sys.exit(0)
                sys.exit(2)
                """)
            )
        os.chmod(script_path, 0o755)

        proc = start_server({"MEMSEARCH_BIN": script_path, "MEMSEARCH_PROJECT_DIR": tmpdir})
        try:
            send_request(proc, {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}})
            resp = send_request(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 13,
                    "method": "tools/call",
                    "params": {"name": "memsearch_search", "arguments": {"query": "x"}},
                },
            )
            parsed = json.loads(resp["result"]["content"][0]["text"])
            check("cwd is project dir", parsed[0]["cwd"] == tmpdir)
        finally:
            proc.terminate()
            proc.wait(timeout=5)


def test_stderr_not_on_stdout() -> None:
    """Stderr from subprocess never appears in tool result text (only in error struct)."""
    print("\n[test_stderr_not_on_stdout]")
    with tempfile.TemporaryDirectory() as tmpdir:
        fake_bin = make_fake_memsearch_error(tmpdir, exit_code=1, stderr="SECRET LEAK")
        proc = start_server({"MEMSEARCH_BIN": fake_bin, "MEMSEARCH_PROJECT_DIR": tmpdir})
        try:
            send_request(proc, {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}})
            resp = send_request(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 14,
                    "method": "tools/call",
                    "params": {"name": "memsearch_search", "arguments": {"query": "x"}},
                },
            )
            text = resp["result"]["content"][0]["text"]
            # The raw stderr should appear in the JSON error struct, not leaked as raw text
            parsed = json.loads(text)
            check("stderr in error struct", parsed.get("stderr") == "SECRET LEAK")
            # Make sure there's no raw stderr prefix/suffix outside JSON
            check("content is pure JSON", text.strip().startswith("{"))
            # Additionally, read the server's actual stderr stream
            # (make sure diagnostic logs are on stderr, not stdout)
            proc.stdin.close()
            proc.wait(timeout=5)
            server_stderr = proc.stderr.read()
            check("server diagnostics on stderr", "memsearch-mcp" in server_stderr.lower())
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=5)


# ── runner ──────────────────────────────────────────────────────────────────

def main() -> None:
    print("=" * 60)
    print("memsearch-mcp tests")
    print("=" * 60)

    test_initialize()
    test_tools_list()
    test_search_success()
    test_search_with_source_prefix()
    test_expand_success()
    test_expand_with_lines()
    test_missing_executable()
    test_backend_error()
    test_empty_query_rejected()
    test_invalid_top_k_rejected()
    test_empty_chunk_hash_rejected()
    test_unknown_tool()
    test_backend_empty_output()
    test_backend_non_json_output()
    test_project_dir_is_passed()
    test_stderr_not_on_stdout()

    print(f"\n{'=' * 60}")
    print(f"Results: {passed} passed, {failed} failed, {passed + failed} total")
    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
