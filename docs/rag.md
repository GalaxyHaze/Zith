# Local RAG for Zith

`scripts/rag.py` is a daemon-free, local hybrid search index for the Zith
workspace. It is intended to work inside sandboxes where
`opencode-rag`/Ollama cannot open localhost sockets or write outside the
workspace.

## Why this exists

The existing OpenCodeRAG index stores code chunks, but search currently
requires Ollama to compute query embeddings at request time. In ByteAsk's
sandbox that connection is blocked, and Ollama is often not running. This tool
does the same job with a Python-only BM25 index and optional local embeddings
through `llama.cpp`.

## Commands

```bash
# Build both indexes (code + docs) into .rag-index/
python3 scripts/rag.py index

# Search source code
python3 scripts/rag.py search "CompilationSession pipeline" --kind code --json

# Search markdown/docs
python3 scripts/rag.py search "NRA boundaries" --kind docs --json

# Inspect an indexed chunk
python3 scripts/rag.py show <chunk_id> --kind code

# Index summary
python3 scripts/rag.py stats
```

Use `--root` and `--index-dir` to point at another checkout. Search results
are deterministic JSON records with `path`, `start_line`, `end_line`, `title`,
`score`, and `content`.

## Optional embeddings

If [llama.cpp](https://github.com/ggml-org/llama.cpp) is built and a GGUF
embedding model exists, pass both to the search command:

```bash
python3 scripts/rag.py search "semantic parser" \
  --embedder /home/diogo/llama.cpp/build/bin/llama-embedding \
  --model /home/diogo/.ollama/models/blobs/sha256-06507c7b42688469c4e7298b0a1e16deff06caf291cf0a5b278c308249c3e439
```

Embeddings are a hybrid re-rank layer on top of BM25. If the binary or model
is unavailable, search degrades gracefully to keyword/BM25 and never blocks
the sandbox.

## ByteAsk MCP integration

The MCP adapter exposes three read-only tools:

```bash
python3 scripts/rag-mcp.py
```

Standard byteask registration command, run from the repository root:

```bash
byteask mcp add rag \
  --env RAG_PROJECT_DIR=/home/diogo/Zith \
  -- python3 /home/diogo/Zith/scripts/rag-mcp.py
```

The tools are `rag_search`, `rag_show`, and `rag_stats`. They only spawn the
CLI locally; no network socket is opened.

## Index layout

Two independent corpora are built:

- `code`: `.c`, `.h`, `.cpp`, `.cc`, `.cxx`, `.hpp`, `.hxx`, `.zith`
- `docs`: `.md`, `.markdown` under the project root

Build artifacts, vendored code, `.git`, `.opencode`, and other noisy
directories are excluded. Chunks are line-anchored so search results point at
the exact file range to read.

## Tests

```bash
python3 scripts/test-rag.py
```

The suite creates a tiny synthetic repository, indexes it, and verifies code
and docs search, `show`, CLI stats, and MCP JSON-RPC behavior.
