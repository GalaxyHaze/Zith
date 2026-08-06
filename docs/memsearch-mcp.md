# Memsearch MCP Integration for ByteAsk

## What It Is

A thin MCP stdio adapter that exposes `memsearch` semantic memory search as two
read-only tools inside ByteAsk.  It does **not** replace `opencode-rag` — the
two serve different purposes:

| Server | Purpose |
|---|---|
| `opencode-rag` | Source-code semantic search (C++, headers, `.zith` files) |
| `memsearch` | Project memory / Markdown knowledge-base search (docs, spec, notes) |

The adapter delegates entirely to the installed `memsearch` CLI.  It does not
import `memsearch` internals, modify the Milvus database, or re-implement
embeddings.

## Tools Exposed

### `memsearch_search`

Semantic search across indexed Markdown knowledge bases.  Returns ranked chunks
with `content`, `source`, `heading`, `score`, and `chunk_hash`.

### `memsearch_expand`

Progressive disclosure: given a `chunk_hash` from a search result, returns the
full Markdown heading section (or N lines of surrounding context).

## Prerequisites

1. **Install memsearch:**

   ```bash
   pip install memsearch[all]
   # or for ONNX-only local embeddings:
   pip install memsearch[onnx]
   ```

2. **Index your project's Markdown files:**

   ```bash
   memsearch index /path/to/project --max-chunk-size 1500
   ```

   Run this whenever docs change.  The index is stored in `~/.memsearch/`.

3. **Verify indexing:**

   ```bash
   memsearch stats
   memsearch search "parser architecture" --top-k 3 --json-output
   ```

## ByteAsk Registration

From the project root:

```bash
byteask mcp add memsearch \
  --env MEMSEARCH_BIN=/home/diogo/.local/bin/memsearch \
  --env MEMSEARCH_PROJECT_DIR=/home/diogo/Zith \
  -- python3 /home/diogo/Zith/scripts/memsearch-mcp.py
```

Replace the absolute paths with values matching your machine.

## Verification

```bash
byteask mcp list
# Should show: memsearch  python3 ... scripts/memsearch-mcp.py  enabled

byteask mcp get memsearch --json
```

Inside a ByteAsk session:

> Search my project memory for how the compiler pipeline stages are connected.

## Common Failures

| Symptom | Cause | Fix |
|---|---|---|
| `memsearch exited with code 1` + `milvus` in stderr | Milvus Lite database locked or incompatible version | Delete `~/.memsearch/milvus.db` and re-index: `memsearch index /path/to/project --force` |
| `memsearch executable not found` | `memsearch` not in PATH | Set `MEMSEARCH_BIN` env var or install memsearch |
| `memsearch timed out` | Embedding model download on first run | Run `memsearch search "test"` once outside ByteAsk to warm up; increase `MEMSEARCH_TIMEOUT_SEC` |
| Empty results for known content | Index out of date | Re-run `memsearch index` |
| `chunk not found` during expand | Chunk was deleted or re-indexed | Re-run the search to get fresh `chunk_hash` values |
| `Read-only file system` on `~/.memsearch` | Sandbox restricts home directory writes | Set `MEMSEARCH_PROJECT_DIR` or move the Milvus database to a writable path via `memsearch config` |

## Architecture

```
ByteAsk  ──(MCP stdio)──>  scripts/memsearch-mcp.py  ──(subprocess)──>  memsearch CLI
                                                                          │
                                                                     Milvus Lite
                                                                     (local .db)
```

The adapter:
- Validates all inputs before invoking `memsearch`.
- Captures stderr separately; never leaks it into tool output.
- Returns structured MCP errors on any failure.
- Never modifies files, the index, or embeddings.

## Environment Variables

See [scripts/memsearch-mcp.env.example](/home/diogo/Zith/scripts/memsearch-mcp.env.example) for a documented template.

| Variable | Default | Purpose |
|---|---|---|
| `MEMSEARCH_BIN` | `memsearch` (from PATH) | Path to the memsearch executable |
| `MEMSEARCH_PROJECT_DIR` | cwd | Working directory for subprocess invocation |
| `MEMSEARCH_TIMEOUT_SEC` | `30` | Subprocess timeout in seconds |
