# Validated C ABI Surface

Only C declarations whose ABI is verified by the Zith C binder are imported; everything outside the validated surface is rejected with a clear diagnostic. Struct-by-value is considered essential and will be implemented with verified simple-record ABI, while bitfields, packing, flexible arrays, and other unverified layouts remain rejected until validated.

Status: proposed

Considered Options:

- Import broad C syntax and let users discover ABI bugs at runtime. This maximizes convenience but contradicts compiler reliability goals.
- Import only scalars/pointers today and reject records entirely. This is safe, but it blocks the struct-by-value use cases the user considers essential.
- Validate a documented subset, including simple records by value, and reject everything not yet proven. This keeps user-facing promises truthful while enabling the important C interop surface.

Consequences:

- The binder must carry per-target ABI checks for supported record layouts and reject detectable unsupported cases (bitfields, packing, anonymous layouts, flexible arrays).
- Old behavior where unsupported declarations are skipped must move to explicit diagnostics for records that require ABI proof.
- Tests must include target-specific ABI expectations and negative cases proving unsupported records fail early.
