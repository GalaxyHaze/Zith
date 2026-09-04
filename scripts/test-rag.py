#!/usr/bin/env python3
"""Tests for scripts/rag.py and scripts/rag-mcp.py.

Run:
    python3 scripts/test-rag.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path


SCRIPTS = Path(__file__).resolve().parent
ROOT = SCRIPTS.parent
RAG = SCRIPTS / "rag.py"
RAG_MCP = SCRIPTS / "rag-mcp.py"
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
        print(f"  FAIL  {name}" + (f"  | {detail}" if detail else ""))


def run_cli(*args: str, cwd: str | None = None, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    full_env = os.environ.copy()
    if env:
        full_env.update(env)
    return subprocess.run(
        [PYTHON, str(RAG), *args],
        capture_output=True,
        text=True,
        cwd=cwd or str(ROOT),
        env=full_env,
        timeout=30,
    )


def make_small_repo(root: str) -> None:
    src = Path(root) / "src"
    docs = Path(root) / "docs"
    src.mkdir(parents=True)
    docs.mkdir(parents=True)
    (src / "pipeline.cpp").write_text(
        "// CompilationSession begins here\n"
        "class CompilationSession {\n"
        "    void runPipeline() { /* semantic parser */ }\n"
        "};\n"
        "void lowerHir() { // hello world\n"
        "  const int E4008 = 1;\n"
        "}\n"
    )
    (src / "lexer.hpp").write_text(
        "#pragma once\n"
        "struct Lexer { int scan(); };\n"
    )
    (docs / "architecture.md").write_text(
        "# Architecture\n\n"
        "The CompilationSession orchestrates the pipeline.\n\n"
        "## Sema\n\nThe semantic parser validates Zith-- bindings.\n"
    )


def test_index_and_search_code() -> None:
    print("\n[test_index_and_search_code]")
    with tempfile.TemporaryDirectory() as tmp:
        make_small_repo(tmp)
        index = os.path.join(tmp, ".rag-index")
        proc = run_cli("index", "--root", tmp, "--index-dir", index)
        check("index exits 0", proc.returncode == 0, proc.stderr)
        try:
            data = json.loads(proc.stdout)
        except json.JSONDecodeError:
            data = {}
        check("index has code", data.get("kinds", {}).get("code", 0) >= 2, proc.stdout)
        check("index has docs", data.get("kinds", {}).get("docs", 0) >= 1)

        proc = run_cli("search", "CompilationSession pipeline", "--kind", "code", "--top-k", "3", "--json", "--root", tmp, "--index-dir", index)
        check("search exits 0", proc.returncode == 0, proc.stderr)
        result = json.loads(proc.stdout)
        paths = [r["path"] for r in result.get("results", [])]
        check("finds pipeline.cpp", "src/pipeline.cpp" in paths, repr(paths))


def test_docs_search() -> None:
    print("\n[test_docs_search]")
    with tempfile.TemporaryDirectory() as tmp:
        make_small_repo(tmp)
        index = os.path.join(tmp, ".rag-index")
        run_cli("index", "--root", tmp, "--index-dir", index)
        proc = run_cli("search", "Sema bindings", "--kind", "docs", "--top-k", "3", "--json", "--root", tmp, "--index-dir", index)
        check("docs search exits 0", proc.returncode == 0, proc.stderr)
        result = json.loads(proc.stdout)
        paths = [r["path"] for r in result.get("results", [])]
        check("finds architecture.md", "docs/architecture.md" in paths, repr(paths))


def test_stats() -> None:
    print("\n[test_stats]")
    with tempfile.TemporaryDirectory() as tmp:
        make_small_repo(tmp)
        index = os.path.join(tmp, ".rag-index")
        run_cli("index", "--root", tmp, "--index-dir", index)
        proc = run_cli("stats", "--root", tmp, "--index-dir", index)
        check("stats exits 0", proc.returncode == 0, proc.stderr)
        result = json.loads(proc.stdout)
        check("stats reports code chunks", result["kinds"]["code"]["chunks"] >= 2, proc.stdout)


def test_show_chunk() -> None:
    print("\n[test_show_chunk]")
    with tempfile.TemporaryDirectory() as tmp:
        make_small_repo(tmp)
        index = os.path.join(tmp, ".rag-index")
        run_cli("index", "--root", tmp, "--index-dir", index)
        proc = run_cli("search", "lexer.hpp", "--kind", "code", "--top-k", "1", "--json", "--root", tmp, "--index-dir", index)
        result = json.loads(proc.stdout)
        chunk_id = result["results"][0]["chunk_id"]
        check("result has chunk_id", bool(chunk_id))
        show = run_cli("show", chunk_id, "--kind", "code", "--root", tmp, "--index-dir", index)
        show_result = json.loads(show.stdout)
        check("show returns the real path", show_result["path"] == "src/lexer.hpp", repr(show_result))


def send_request(proc: subprocess.Popen, req: dict) -> dict:
    proc.stdin.write(json.dumps(req, ensure_ascii=False) + "\n")
    proc.stdin.flush()
    line = proc.stdout.readline()
    if not line:
        raise RuntimeError(f"No response for {req.get('method')}")
    return json.loads(line)


def start_mcp(env: dict[str, str] | None = None, project: str | None = None) -> subprocess.Popen:
    full_env = os.environ.copy()
    if env:
        full_env.update(env)
    if project:
        full_env.setdefault("RAG_PROJECT_DIR", project)
    proc = subprocess.Popen(
        [PYTHON, str(RAG_MCP)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=full_env,
    )
    time.sleep(0.15)
    return proc


def test_mcp_tools_list() -> None:
    print("\n[test_mcp_tools_list]")
    proc = start_mcp()
    try:
        send_request(proc, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
        resp = send_request(proc, {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
        names = {t["name"] for t in resp["result"]["tools"]}
        check("rag_search listed", "rag_search" in names)
        check("rag_stats listed", "rag_stats" in names)
    finally:
        proc.terminate()
        proc.wait(timeout=5)


def test_mcp_search_with_project() -> None:
    print("\n[test_mcp_search_with_project]")
    with tempfile.TemporaryDirectory() as tmp:
        make_small_repo(tmp)
        index = os.path.join(tmp, ".rag-index")
        run_cli("index", "--root", tmp, "--index-dir", index)
        proc = start_mcp(env={"RAG_INDEX_DIR": index}, project=tmp)
        try:
            send_request(proc, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
            resp = send_request(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "tools/call",
                    "params": {
                        "name": "rag_search",
                        "arguments": {"query": "CompilationSession", "kind": "code", "top_k": 3},
                    },
                },
            )
            result = resp["result"]
            check("search not isError", not result.get("isError"), str(result)[:400])
            parsed = json.loads(result["content"][0]["text"])
            check("has results", parsed.get("count", 0) >= 1, str(parsed)[:400])
        finally:
            proc.terminate()
            proc.wait(timeout=5)


def test_mcp_invalid_query() -> None:
    print("\n[test_mcp_invalid_query]")
    proc = start_mcp()
    try:
        send_request(proc, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
        resp = send_request(
            proc,
            {
                "jsonrpc": "2.0",
                "id": 4,
                "method": "tools/call",
                "params": {"name": "rag_search", "arguments": {"query": "   "}},
            },
        )
        check("invalid query isError", resp["result"].get("isError") is True)
        text = json.loads(resp["result"]["content"][0]["text"])
        check("invalid query message", "query" in text.get("error", "").lower())
    finally:
        proc.terminate()
        proc.wait(timeout=5)


def test_mcp_stats() -> None:
    print("\n[test_mcp_stats]")
    with tempfile.TemporaryDirectory() as tmp:
        make_small_repo(tmp)
        index = os.path.join(tmp, ".rag-index")
        run_cli("index", "--root", tmp, "--index-dir", index)
        proc = start_mcp(env={"RAG_INDEX_DIR": index}, project=tmp)
        try:
            send_request(proc, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
            resp = send_request(proc, {"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "rag_stats", "arguments": {}}})
            parsed = json.loads(resp["result"]["content"][0]["text"])
            check("stats has code chunks", parsed["kinds"]["code"]["chunks"] >= 2, str(parsed))
        finally:
            proc.terminate()
            proc.wait(timeout=5)


def test_mcp_show() -> None:
    print("\n[test_mcp_show]")
    with tempfile.TemporaryDirectory() as tmp:
        make_small_repo(tmp)
        index = os.path.join(tmp, ".rag-index")
        run_cli("index", "--root", tmp, "--index-dir", index)
        proc = start_mcp(env={"RAG_INDEX_DIR": index}, project=tmp)
        try:
            send_request(proc, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
            resp = send_request(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 6,
                    "method": "tools/call",
                    "params": {"name": "rag_show", "arguments": {"chunk_id": "lexer.hpp", "kind": "code"}},
                },
            )
            result = resp["result"]
            check("show not isError", not result.get("isError"), str(result)[:400])
            parsed = json.loads(result["content"][0]["text"])
            check("show has path", parsed.get("path") == "src/lexer.hpp", str(parsed)[:400])
        finally:
            proc.terminate()
            proc.wait(timeout=5)


def main() -> None:
    print("=" * 60)
    print("rag tests")
    print("=" * 60)
    test_index_and_search_code()
    test_docs_search()
    test_stats()
    test_show_chunk()
    test_mcp_tools_list()
    test_mcp_search_with_project()
    test_mcp_invalid_query()
    test_mcp_stats()
    test_mcp_show()
    print(f"\n{'=' * 60}")
    print(f"Results: {passed} passed, {failed} failed, {passed + failed} total")
    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
