# C Macro Constants: Imported Scalars With End-to-End Verification

Object-like scalar macros from C headers are parsed into `cinterop::Constant` values and lowered by the modern pipeline to immutable globals. This was verified through the CLI with a local header:

```c
#define ANSWER 42
#define RATIO 3.5
#define LETTER 'A'
#define BIG 2147483647
```

`import "constants.h"` followed by a reference to each macro passes `zithc check`; `ANSWER` compiled with `zithc run` and returned the expected value.

Status: accepted

Considered Options:

The alternative is to advertise the feature as fully working while relying only on unit tests. The pipeline test covers lowering, but the user-facing behavior deserves at least one end-to-end example and an explicit status note so an integration regression is not mistaken for a design gap.

Consequences:

- `docs/impl-status.md` keeps a precise note that C macro constants are supported by the parser/lowering path and verified through the CLI.
- If the end-to-end smoke test fails for reasons unrelated to the import model, it is recorded as a follow-up rather than silently changing the feature status.
