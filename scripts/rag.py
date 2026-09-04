#!/usr/bin/env python3
"""Local, daemon-free RAG search for the Zith workspace.

The tool builds two independent indexes:
  - code: C++, headers, and Zith sources
  - docs: markdown files in docs/ and memory/

Ranking is BM25-style with a small metadata boost. Optional embeddings are
computed with llama.cpp when `--embedder` points to a working executable;
without one the search is fully keyword based.

Commands:
  scripts/rag.py index [--embedder /path/llama-embedding] [--model path.gguf]
  scripts/rag.py search <query> [--kind code|docs] [--top-k N] [--json]
  scripts/rag.py stats
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable


DEFAULT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_INDEX_DIR = DEFAULT_ROOT / ".rag-index"

CODE_EXTS = frozenset({".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".hxx", ".zith"})
SOURCE_EXTS = frozenset({".md", ".markdown"})

EXCLUDE_DIRS = frozenset(
    {
        ".git",
        ".opencode",
        ".rag-index",
        ".codex",
        ".agents",
        "build",
        "build-debug",
        "build-wasm",
        "dist",
        "target",
        "node_modules",
        "vendor",
        "_deps",
        "__pycache__",
        "tmp-tests",
    }
)

META_DOCS = frozenset({"docs/rag.md"})

CHARS_PER_TOKEN = 4


@dataclass
class Chunk:
    path: str
    kind: str
    start_line: int
    end_line: int
    content: str
    title: str = ""
    chunk_id: str = ""


@dataclass
class Corpus:
    kind: str
    chunks: list[Chunk] = field(default_factory=list)
    df: Counter[str] = field(default_factory=Counter)
    doc_len: list[int] = field(default_factory=list)


def _is_excluded(root: Path, path: Path) -> bool:
    rel = path.relative_to(root)
    return any(part in EXCLUDE_DIRS for part in rel.parts)


def _iter_files(root: Path, exts: frozenset[str]) -> Iterable[Path]:
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(
            d for d in dirnames if d not in EXCLUDE_DIRS and not d.startswith(".")
        )
        for name in sorted(filenames):
            path = Path(dirpath) / name
            if path.suffix.lower() in exts and not _is_excluded(root, path):
                rel = path.relative_to(root)
                if str(rel).replace("\\", "/") not in META_DOCS:
                    yield path


def _read_text(path: Path) -> str:
    for encoding in ("utf-8", "latin-1"):
        try:
            return path.read_text(encoding=encoding)
        except UnicodeDecodeError:
            continue
    return path.read_text(encoding="utf-8", errors="replace")


def _line_count(text: str) -> int:
    if not text:
        return 1
    return text.count("\n") + 1


def _markdown_title(lines: list[str], start: int) -> str:
    for i in range(start, min(len(lines), start + 6)):
        stripped = lines[i].strip()
        if stripped.startswith("#"):
            return stripped.lstrip("# ").strip()[:200]
    return ""


def split_docs(text: str, max_chars: int = 1200) -> list[str]:
    """Split markdown into heading-delimited chunks, falling back to paragraphs."""
    lines = text.splitlines()
    if not lines:
        return [""]
    chunks: list[str] = []
    current: list[str] = []
    current_len = 0

    def flush() -> None:
        nonlocal current, current_len
        if current:
            chunks.append("\n".join(current).strip())
        current = []
        current_len = 0

    for line in lines:
        if line.startswith("#") and current_len > 0:
            flush()
        current.append(line)
        current_len += len(line) + 1
        if current_len >= max_chars and len(current) > 5:
            flush()
    flush()

    if len(chunks) <= 1:
        return ["\n".join(lines).strip()]

    # Reflow chunks that are only a heading followed by empty text.
    merged: list[str] = []
    for chunk in chunks:
        if merged and len(merged[-1]) < 80:
            merged[-1] += "\n" + chunk
        else:
            merged.append(chunk)
    return [c for c in merged if c.strip()]


def split_code(text: str, max_chars: int = 1400) -> list[str]:
    """Split code by top-level declaration-like boundaries when practical."""
    lines = text.splitlines()
    if not lines:
        return [""]
    chunks: list[str] = []
    current: list[str] = []
    current_len = 0
    in_braces = 0

    def flush() -> None:
        nonlocal current, current_len
        if current:
            chunks.append("\n".join(current).strip())
        current = []
        current_len = 0

    for line in lines:
        # Track braces roughly to avoid splitting inside a function/class body.
        stripped = line.strip()
        if stripped:
            for ch in stripped:
                if ch == "{":
                    in_braces += 1
                elif ch == "}":
                    in_braces -= 1
        current.append(line)
        current_len += len(line) + 1
        should_break = (
            current_len >= max_chars
            and in_braces <= 0
            and (stripped.endswith(";") or stripped.endswith("})"))
        )
        if should_break:
            flush()
    flush()
    return [c for c in chunks if c.strip()]


def tokenize(text: str) -> list[str]:
    """Tokenize source/doc text, preserving useful identifiers with separators."""
    text = text.lower()
    tokens: list[str] = []
    for part in re.findall(r"[a-z0-9_]+|(?:[a-z0-9_]+(?:[::_-]+[a-z0-9_]+)+)", text):
        tokens.append(part)
        # Also emit the sub-parts of identifiers so `E4008` and `CompilationSession`
        # can be matched by their stable fragments.
        if "::" in part or "_" in part or "-" in part:
            for frag in re.split(r"[::_\-]+", part):
                if frag:
                    tokens.append(frag)
    return tokens


def _build_chunks(root: Path, kind: str, exts: frozenset[str]) -> list[Chunk]:
    chunks: list[Chunk] = []
    for path in _iter_files(root, exts):
        text = _read_text(path)
        rel = str(path.relative_to(root))
        if kind == "docs":
            pieces = split_docs(text)
        else:
            pieces = split_code(text)
        offset = 0
        for piece in pieces:
            start_line = text.count("\n", 0, offset) + 1 if offset else 1
            end_line = start_line + piece.count("\n")
            title = _markdown_title(text.splitlines(), start_line - 1) if kind == "docs" else ""
            digest = hashlib.sha1(f"{rel}\0{start_line}\0{end_line}\0{piece}".encode()).hexdigest()[:16]
            chunks.append(
                Chunk(
                    path=rel,
                    kind=kind,
                    start_line=start_line,
                    end_line=end_line,
                    content=piece,
                    title=title,
                    chunk_id=digest,
                )
            )
            offset = max(0, text.find(piece, offset))
            if offset == -1:
                offset = len(text)
            else:
                offset += len(piece)
    return chunks


def bm25_score(doc_tokens: list[str], corpus: Corpus, query_tokens: list[str], avgdl: float, k1: float = 1.5, b: float = 0.75) -> float:
    score = 0.0
    doc_len = len(doc_tokens)
    counts = Counter(doc_tokens)
    n = len(corpus.chunks)
    for term in set(query_tokens):
        freq = counts.get(term, 0)
        if freq == 0:
            continue
        df = corpus.df.get(term, 0)
        idf = math.log(1.0 + (n - df + 0.5) / (df + 0.5)) if df else math.log(1.0 + n)
        tf_part = freq * (k1 + 1.0)
        denom = freq + k1 * (1.0 - b + b * doc_len / avgdl) if avgdl else freq
        score += idf * tf_part / denom
    return score


def _build_index(root: Path, index_dir: Path, kinds: Iterable[str] | None = None) -> dict[str, Corpus]:
    wanted = set(kinds or ("code", "docs"))
    result: dict[str, Corpus] = {}
    for kind in wanted:
        exts = CODE_EXTS if kind == "code" else SOURCE_EXTS
        chunks = _build_chunks(root, kind, exts)
        df: Counter[str] = Counter()
        doc_len: list[int] = []
        for chunk in chunks:
            terms = set(tokenize(chunk.content))
            df.update(terms)
            doc_len.append(len(tokenize(chunk.content)))
        result[kind] = Corpus(kind=kind, chunks=chunks, df=df, doc_len=doc_len)

    index_dir.mkdir(parents=True, exist_ok=True)
    manifest = {
        "version": 1,
        "root": str(root),
        "kinds": sorted(result),
        "chunk_counts": {kind: len(corpus.chunks) for kind, corpus in result.items()},
    }
    (index_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))
    return result


def _save_index(index_dir: Path, corpora: dict[str, Corpus]) -> None:
    for kind, corpus in corpora.items():
        payload = {
            "kind": kind,
            "chunks": [
                {
                    "path": c.path,
                    "kind": c.kind,
                    "start_line": c.start_line,
                    "end_line": c.end_line,
                    "content": c.content,
                    "title": c.title,
                    "chunk_id": c.chunk_id,
                }
                for c in corpus.chunks
            ],
            "df": dict(corpus.df.most_common()),
            "doc_len": corpus.doc_len,
        }
        (index_dir / f"{kind}.json").write_text(json.dumps(payload))


def _load_index(index_dir: Path) -> dict[str, Corpus]:
    result: dict[str, Corpus] = {}
    manifest_path = index_dir / "manifest.json"
    if not manifest_path.exists():
        return result
    manifest = json.loads(manifest_path.read_text())
    for kind in manifest.get("kinds", []):
        path = index_dir / f"{kind}.json"
        if not path.exists():
            continue
        data = json.loads(path.read_text())
        chunks = [Chunk(**{k: v for k, v in c.items() if k in Chunk.__dataclass_fields__}) for c in data.get("chunks", [])]
        result[kind] = Corpus(
            kind=kind,
            chunks=chunks,
            df=Counter(data.get("df", {})),
            doc_len=list(data.get("doc_len", [])),
        )
    return result


def _metadata_boost(chunk: Chunk, tokens: list[str]) -> float:
    boost = 0.0
    lower_path = chunk.path.lower()
    lower_title = chunk.title.lower()
    for term in tokens:
        if term in lower_path.replace("/", " ").replace("_", " ").replace("-", " "):
            boost += 0.5
        if term in lower_title:
            boost += 0.7
    return boost


def search(
    corpora: dict[str, Corpus],
    query: str,
    kind: str,
    top_k: int,
    embedder: str | None = None,
    model: str | None = None,
) -> dict[str, Any]:
    corpus = corpora.get(kind)
    if corpus is None or not corpus.chunks:
        return {"error": f"no index for kind={kind!r}; run scripts/rag.py index", "results": []}
    query_tokens = tokenize(query)
    if not query_tokens:
        return {"error": "query did not contain searchable terms", "results": []}
    avgdl = sum(corpus.doc_len) / len(corpus.doc_len) if corpus.doc_len else 1.0
    scored: list[tuple[float, int]] = []
    for idx, chunk in enumerate(corpus.chunks):
        tokens = tokenize(chunk.content)
        score = bm25_score(tokens, corpus, query_tokens, avgdl)
        score += _metadata_boost(chunk, query_tokens)
        if score > 0:
            scored.append((score, idx))
    scored.sort(key=lambda item: (-item[0], item[1]))

    if embedder and model and os.path.isfile(embedder) and os.path.isfile(model):
        try:
            sem_scores = _semantic_rerank(embedder, model, query, corpus, scored, top_k)
            for chunk_idx, sem in sem_scores:
                if sem <= 0:
                    continue
                for i, (score, idx) in enumerate(scored):
                    if idx == chunk_idx:
                        scored[i] = (score * 0.7 + sem * 3.0, chunk_idx)
                        break
            scored.sort(key=lambda item: (-item[0], item[1]))
        except Exception as exc:  # embeddings are optional; never break BM25 search
            print(f"[rag] embeddings skipped: {exc}", file=sys.stderr)

    results = []
    for score, idx in scored[:top_k]:
        chunk = corpus.chunks[idx]
        results.append(
            {
                "chunk_id": chunk.chunk_id,
                "path": chunk.path,
                "kind": chunk.kind,
                "start_line": chunk.start_line,
                "end_line": chunk.end_line,
                "title": chunk.title,
                "score": round(score, 4),
                "content": chunk.content[:4000],
            }
        )
    return {"results": results, "count": len(results)}


def _parse_raw_embeddings(raw: str, count: int) -> list[list[float]] | None:
    raw = raw.strip()
    if not raw:
        return None
    try:
        data = json.loads(raw)
        if isinstance(data, list) and data and isinstance(data[0], list):
            return data
    except json.JSONDecodeError:
        pass
    lines = [line.strip() for line in raw.splitlines() if line.strip()]
    if len(lines) < count:
        return None
    parsed: list[list[float]] = []
    for line in lines[:count]:
        parsed.append([float(v) for v in line.split()])
    return parsed if parsed and parsed[0] else None


def _embed_texts(embedder: str, model: str, texts: list[str]) -> list[list[float]]:
    if not texts:
        return []
    encoded: list[str] = []
    for text in texts:
        compact = " ".join(text.split())
        encoded.append(compact[:3000].replace("<#sep#>", " ").replace("\n", " "))
    joined = "<#sep#>".join(encoded)
    if not joined:
        return []
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".txt", delete=False) as f:
        f.write(joined)
        prompt_path = f.name
    try:
        proc = subprocess.run(
            [
                embedder,
                "-m",
                model,
                "-p",
                joined,
                "--embd-separator",
                "<#sep#>",
                "--embd-output-format",
                "raw",
                "-n",
                "0",
            ],
            capture_output=True,
            text=True,
            timeout=120,
        )
        parsed = _parse_raw_embeddings(proc.stdout, len(encoded))
        if parsed is None:
            raise RuntimeError(f"llama-embedding returned unusable output: {proc.stderr[:200]}")
        return parsed
    finally:
        try:
            os.unlink(prompt_path)
        except OSError:
            pass


def _cosine(a: list[float], b: list[float]) -> float:
    if len(a) != len(b) or not a:
        return 0.0
    dot = sum(x * y for x, y in zip(a, b))
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(y * y for y in b))
    if not na or not nb:
        return 0.0
    return dot / (na * nb)


def _semantic_rerank(
    embedder: str,
    model: str,
    query: str,
    corpus: Corpus,
    scored: list[tuple[float, int]],
    top_k: int,
) -> list[tuple[int, float]]:
    """Re-rank the best BM25 candidates using dense vectors."""
    candidate_limit = min(max(top_k * 2, 4), 40)
    candidates = [corpus.chunks[idx] for _, idx in scored[:candidate_limit]]
    if not candidates:
        return []
    embedded_query = _embed_texts(embedder, model, [query])
    query_vec = embedded_query[0] if embedded_query else []
    if not query_vec:
        return []
    doc_vecs = _embed_texts(embedder, model, [c.content[:3000] for c in candidates])
    if len(doc_vecs) != len(candidates):
        return []
    return [(scored[i][1], _cosine(query_vec, doc_vecs[i])) for i in range(len(candidates))]


def cmd_index(args: argparse.Namespace) -> int:
    root = Path(args.root or os.environ.get("RAG_PROJECT_DIR") or DEFAULT_ROOT).resolve()
    index_dir = Path(args.index_dir or os.environ.get("RAG_INDEX_DIR") or DEFAULT_INDEX_DIR).resolve()
    corpora = _build_index(root, index_dir, kinds=(args.kind,) if args.kind else None)
    _save_index(index_dir, corpora)
    print(
        json.dumps(
            {
                "index_dir": str(index_dir),
                "kinds": {kind: len(corpus.chunks) for kind, corpus in corpora.items()},
            },
            indent=2,
        )
    )
    return 0


def cmd_search(args: argparse.Namespace) -> int:
    root = Path(args.root or os.environ.get("RAG_PROJECT_DIR") or DEFAULT_ROOT).resolve()
    index_dir = Path(args.index_dir or os.environ.get("RAG_INDEX_DIR") or DEFAULT_INDEX_DIR).resolve()
    corpora = _load_index(index_dir)
    if not corpora:
        print(json.dumps({"error": "index not found; run scripts/rag.py index first"}), file=sys.stderr)
        return 2
    kind = args.kind or ("docs" if args.markdown else "code")
    result = search(
        corpora,
        args.query,
        kind,
        args.top_k,
        embedder=args.embedder,
        model=args.model,
    )
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        if result.get("error"):
            print(f"error: {result['error']}", file=sys.stderr)
            return 2
        for item in result["results"]:
            loc = f"{item['path']}:{item['start_line']}-{item['end_line']}"
            print(f"{item['score']:7.3f}  {loc}")
            if item.get("title"):
                print(f"          {item['title']}")
            snippet = item["content"].replace("\n", " ")
            print(f"          {snippet[:160]}")
            print()
    return 0


def cmd_show(args: argparse.Namespace) -> int:
    index_dir = Path(args.index_dir or os.environ.get("RAG_INDEX_DIR") or DEFAULT_INDEX_DIR).resolve()
    corpora = _load_index(index_dir)
    if not corpora:
        print(json.dumps({"error": "index not found; run scripts/rag.py index first"}), file=sys.stderr)
        return 2
    corpus = corpora.get(args.kind or "code")
    if corpus is None:
        print(json.dumps({"error": f"no index for kind={args.kind!r}"}), file=sys.stderr)
        return 2
    wanted = args.chunk_id
    matches = [c for c in corpus.chunks if c.chunk_id == wanted or wanted in c.path]
    if not matches:
        print(json.dumps({"error": f"chunk not found: {wanted}"}), file=sys.stderr)
        return 2
    chosen = matches[0]
    print(
        json.dumps(
            {
                "chunk_id": chosen.chunk_id,
                "path": chosen.path,
                "kind": chosen.kind,
                "start_line": chosen.start_line,
                "end_line": chosen.end_line,
                "title": chosen.title,
                "content": chosen.content,
            },
            ensure_ascii=False,
            indent=2,
        )
    )
    return 0


def cmd_stats(args: argparse.Namespace) -> int:
    index_dir = Path(args.index_dir or os.environ.get("RAG_INDEX_DIR") or DEFAULT_INDEX_DIR).resolve()
    corpora = _load_index(index_dir)
    if not corpora:
        print(json.dumps({"error": "index not found; run scripts/rag.py index first"}), file=sys.stderr)
        return 2
    print(
        json.dumps(
            {
                "index_dir": str(index_dir),
                "kinds": {
                    kind: {
                        "chunks": len(corpus.chunks),
                        "terms": len(corpus.df),
                    }
                    for kind, corpus in corpora.items()
                },
            },
            indent=2,
        )
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Local daemon-free RAG search for Zith")
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--root", help="project root (default: repo root)")
    common.add_argument("--index-dir", help="index directory (default: .rag-index)")
    parser.add_argument("--root", help=argparse.SUPPRESS)
    parser.add_argument("--index-dir", help=argparse.SUPPRESS)
    sub = parser.add_subparsers(dest="command", required=True)

    p_index = sub.add_parser("index", help="build the local index", parents=[common])
    p_index.add_argument("--kind", choices=("code", "docs"), help="index only one corpus")
    p_index.set_defaults(func=cmd_index)

    p_search = sub.add_parser("search", help="search the local index", parents=[common])
    p_search.add_argument("query")
    p_search.add_argument("--kind", choices=("code", "docs"), help="which corpus to search")
    p_search.add_argument("--markdown", action="store_true", help="shortcut for searching docs")
    p_search.add_argument("--top-k", type=int, default=10)
    p_search.add_argument("--embedder", help="optional llama-embedding executable")
    p_search.add_argument("--model", help="optional embedding GGUF model path")
    p_search.add_argument("--json", action="store_true", help="emit JSON")
    p_search.set_defaults(func=cmd_search)

    p_show = sub.add_parser("show", help="show one indexed chunk", parents=[common])
    p_show.add_argument("chunk_id")
    p_show.add_argument("--kind", choices=("code", "docs"), default="code", help="which corpus to search")
    p_show.set_defaults(func=cmd_show)

    p_stats = sub.add_parser("stats", help="show index stats", parents=[common])
    p_stats.set_defaults(func=cmd_stats)
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
