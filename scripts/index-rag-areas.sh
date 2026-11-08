#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG_DIR="$ROOT/.opencode/rag-areas"

run_area() {
    local area="$1"
    local scope="$2"

    printf '\n=== RAG area: %s (%s) ===\n' "$area" "$scope"
    (
        cd -- "$ROOT/$scope"
        agent-run -t 86400 -c 0 -- opencode-rag index \
            --config "$CONFIG_DIR/$area.json"
    )
}

run_area frontend src
run_area middleend src
run_area backend src
run_area shared src
run_area integrations src
run_area tests tests
run_area examples examples
run_area stdlib stdlib
run_area adrs docs/adr
run_area docs docs
run_area memory memory
