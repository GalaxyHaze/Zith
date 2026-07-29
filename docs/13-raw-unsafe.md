## 13. Raw & Unsafe

> **Implementation status:** `raw fn` is **working**. `raw` as an unwrap operator is **spec-only**.
> `unsafe` blocks, `raw union`, and safety-hierarchy enforcement are **spec-only**.
> See [impl-status.md](impl-status.md).

### 13.1 Safety Hierarchy

```
Safe code (default)
    └─ needs raw blocks to call raw fn
         └─ needs to be inside a raw block / fn to use unsafe block
```

| | `raw` | `unsafe` |
|---|---|---|
| **What it is** | A base-level bypass, always unchecked. | A stronger bypass, valid only inside `raw` contexts. |
| **Scope** | Any expression or statement. | Only inside a `raw fn` or `raw` block. |
| **Debug mode** | Unchecked. | Unchecked. |
| **Release mode** | Unchecked; compiler warns. | Unchecked; compiler warns. |
| **Use case** | C interop, performance-critical paths. | Operations undefined if misused — pointer arithmetic, inline assembly. |

### 13.2 `raw fn`

A `raw fn` bypasses safety checks in both debug and release. The compiler warns if `raw` could be removed in release builds.

```zith
raw fn c_compat(x: raw opaque): raw opaque {
    // basic validations still exist but allows:
    // mut *, use raw opaque / union & etc..
}
```

### 13.3 `unsafe`

```zith
raw fn dangerous(x: opaque) {
    unsafe {
        // extension of raw — completely disables compiler checks
        // allows: bypass mutability, bit_cast, arbitrary address assignment,
        // inline assembly, etc.
        asm {
            mov rax, x         // x is a Zith var — compiler maps it
            call someFn         // can call Zith fns from asm
        }
    }
}
```

### 13.4 `Trust` as a Bridge

`Trust` bridges safe code to raw and unsafe code. A trait extending `Trust` may contain `raw fn` methods that are callable from safe contexts:

```zith
trait Place extends Trust {
    raw fn sample(): i32 {}
}

fn safe_caller(a: impl Place) {
    let v = a.sample();   // allowed: Trust is in scope via Place
}
```

### 13.5 When to Use Each

| Situation | Use |
|---|---|
| C interop, headerless libraries | `raw fn` |
| Pointer arithmetic, inline assembly | `unsafe` inside `raw fn` |
| Exposing low-level operations to safe code | `Trust` capability |
| Normal application code | Neither — stay safe |

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
