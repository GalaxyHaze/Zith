# Lexer Benchmark Results

**Compiler:** GCC 14.2.1 | **Flags:** `-O3 -march=native -ffast-math` | **Mode:** stress (4 MiB, 50 samples)

## Debug vs Release

| Workload | Debug (MiB/s) | Release (MiB/s) | Speedup | Release ns/token |
|---|---|---|---|---|
| numbers_id | 90.54 | **505.06** | 5.6x | 14.87 |
| mixed-valid | 67.68 | **416.56** | 6.2x | 15.11 |
| operators | 58.37 | **285.23** | 4.9x | 8.78 |
| realistic | 44.72 | **230.73** | 5.2x | 11.79 |

## Release Only

| Workload | MiB/s | Mtokens/s | ns/token | Median (ms) | p95 (ms) | Max (ms) |
|---|---|---|---|---|---|---|
| numbers_id | 505.06 | 67.25 | 14.87 | 7.92 | 8.44 | 8.71 |
| mixed-valid | 416.56 | 66.18 | 15.11 | 9.60 | 10.07 | 10.29 |
| operators | 285.23 | 113.94 | 8.78 | 14.02 | 15.12 | 15.62 |
| realistic | 230.73 | 84.84 | 11.79 | 17.34 | 18.92 | 19.27 |

## Notes

- `numbers_id`: Underscore-separated numerics and identifiers — simple automaton cascades, high predictability.
- `mixed-valid`: Doc comments, block comments, strings with escapes, mixed literals. Moderate overhead from escape handling.
- `operators`: Dense multi-character operators. Deepest splits, lowest ns/token due to short tokens.
- `realistic`: Function declarations with keywords, identifiers, punctuation. Most diverse token mix.
