## 13. Raw & Unsafe

> **Status:** see [impl-status.md](impl-status.md) for the current status of the raw and unsafe
> surface.

> **Full-Zith direction:** this chapter describes the Zith-- surface. Full Zith
> replaces the `raw`/`unsafe` function kinds with the `safe`, `trust`, and
> `hazard` levels and the `hazard { }` and `safe { }` blocks. See
> [ADR-0043](adr/0043-full-zith-safety-levels-safe-trust-hazard.md).

### 13.1 Raw Call Marker

In Zith--, `raw fn` marks a callable whose invocation must be visible at each
call site. Write `raw` immediately before a direct or method call:

```zith
struct Device {
    buffer: *u8
}

raw fn readByte(buffer: *u8, index: u64): u8 {
    raw buffer[index]
}

implement Device {
    raw fn readByte(self: lend Device, index: u64): u8 {
        raw self.buffer[index]
    }
}

fn read(buffer: *u8, index: u64): u8 {
    return raw readByte(buffer, index);
}

fn readDevice(device: lend Device): u8 {
    return raw device.readByte(1);
}
```

The marker applies only to the invocation. It does not mark arguments or the
surrounding expression, and it does not disable checks in the function body.
Calling a `raw fn` without the marker is an error (`E2030`). `#[raw("reason")]`
may add a non-empty explanatory note to that diagnostic.

### 13.2 `raw fn`

The declaration marks calls that cross an explicitly visible raw boundary. It
does not make the body unchecked. Function values of raw functions and inferred
raw callable bindings are deferred.

```zith
raw fn c_compat(x: raw opaque): raw opaque {
    // The body is checked like an ordinary function body.
}
```

### 13.3 `unsafe`

`unsafe` is not part of the current Zith-- raw-call contract. Full Zith uses the
separate `safe`/`trust`/`hazard` hierarchy described by
[ADR-0043](adr/0043-full-zith-safety-levels-safe-trust-hazard.md).

### 13.4 External C Functions

In the future, functions imported from C headers will be `raw` by default. Each
call will emit a warning at the call site that the compiler cannot guarantee
the external function's behavioral boundary. The warning remains when the call
is explicitly marked `raw`. An explicit `safe` annotation will make the call
ordinary and suppress the warning. The annotation's syntax is still open.

### 13.5 When to Use Raw Calls

| Situation | Use |
|---|---|
| Calling an explicitly declared raw function | `raw function(args)` |
| Calling an explicitly declared raw method | `raw receiver.method(args)` |
| Calling an imported C-header function | Future default-raw binding and call-site warning |
| Normal application code | An ordinary `fn` call |

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
