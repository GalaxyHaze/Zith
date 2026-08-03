---
name: opencode-rag
description: Semantic code & image retrieval via OpenCodeRAG — vector search, file skeletons, symbol usage lookup, and image description lookup for this workspace
---

## OpenCodeRAG Tools

This workspace has OpenCodeRAG indexed for semantic code and image search. Use these tools BEFORE planning, editing, or answering code questions.

### Decision tree — ALWAYS follow this order

1. User mentions code behavior/architecture → `search_semantic(query)`
2. User mentions a file path → `get_file_skeleton(filePath)` THEN `read` on specific lines
3. User mentions a function/class/variable to edit → `find_usages(symbolName)` THEN `search_semantic` THEN `edit`
4. User asks a code question → `search_semantic` to gather context before answering
5. User asks about an image or visual asset → `describe_image(filePath)` to retrieve its generated description, then optionally `search_semantic` for related code
6. You encounter an error or need a known pitfall → `recall_quirks(query)`
7. You discover a non-obvious fact or workaround → `add_quirk(content)` to persist it
8. A recalled quirk is outdated or wrong → `update_quirk(id, ...)` to fix it, or `delete_quirk(id)` if it no longer applies

### When to use each tool

| Tool | Use when | Example |
|------|----------|---------|
| `search_semantic` | Any code search — find relevant code by meaning or keyword | `"authentication middleware"` |
| `get_file_skeleton` | You have a file path but need to orient before reading | `"src/plugin.ts"` |
| `find_usages` | Before editing any function, class, or variable — check all call sites | `"createRagHooks"` |
| `describe_image` | When the user refers to an image or asks "what's in this screenshot/diagram?" | `"assets/login-screen.png"` |
| `recall_quirks` | You hit an error or need to remember a gotcha, preference, or decision from past sessions | `"lancedb type casting"` |
| `add_quirk` | You just discovered a non-obvious fact, workaround, or convention worth remembering | `'"npm needs --legacy-peer-deps" --type gotcha --tag installation'` |
| `update_quirk` | A recalled quirk is outdated or wrong — fix its content, type, or tags | `id` from `recall_quirks` output + `content: "..."` |
| `delete_quirk` | A quirk is fixed, obsolete, or no longer applies | `id` from `recall_quirks` output |

### Workflow

1. **Skeleton first** — call `get_file_skeleton(filePath)` to see structure
2. **Find usages** — call `find_usages(symbolName)` before modifying any symbol
3. **Search** — call `search_semantic(query)` to find relevant code
4. **Describe images** — call `describe_image(filePath)` when context involves an image file
5. **Read** — use the `read` tool on specific line ranges identified above
6. **Edit** — now you have full context to make safe changes

### Anti-patterns — NEVER do these

- Reading full files without calling `get_file_skeleton` first (wastes tokens)
- Editing a function without calling `find_usages` first (breaks call sites)
- Answering code questions without calling `search_semantic` first (you guess at behavior)
- Using `grep`/`glob` when `search_semantic` would find the answer faster
- Treating image files as text — use `describe_image` instead of reading raw bytes

### Parameters

- `search_semantic`: `query` (req), `pathHints?`, `languageHints?`, `topK?`
- `get_file_skeleton`: `filePath` (req)
- `find_usages`: `symbolName` (req), `pathHint?`, `topK?`
- `describe_image`: `filePath` (req)
- `recall_quirks`: `query` (req), `topK?`, `quirkType?`, `tags?`
- `add_quirk`: `content` (req), `quirkType?`, `tags?`, `sourceRef?`
- `update_quirk`: `id` (req) + at least one of `content?`, `quirkType?`, `tags?`, `confidence?`, `sourceRef?`
- `delete_quirk`: `id` (req)

### Tips

- Use `pathHints` to narrow searches to specific directories
- Use `languageHints` to filter by file type
- `find_usages` is essential before refactoring — it shows every reference
- If no results appear, the workspace may not be indexed yet — run `opencode-rag index`
- Image descriptions are generated at index time using the configured vision provider; ensure `imageDescription` is configured in `opencode-rag.json` if your project includes images
