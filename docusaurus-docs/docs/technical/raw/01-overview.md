---
id: overview
title: Raw & Unsafe Overview
sidebar_label: Overview
description: Introduction to raw operations and unsafe code in Zith.
---

# Raw & Unsafe

:::warning Advanced Topic

This section covers low-level operations that bypass Zith's safety guarantees. Only use these when absolutely necessary and when you fully understand the implications.

:::

## What is "Raw & Unsafe"?

Zith's safety model handles 95% of use cases. The `raw` and `unsafe` features are reserved for situations where you need:

1. **FFI (Foreign Function Interface)** - Calling C libraries directly
2. **Raw pointer arithmetic** - Custom memory layouts and allocators
3. **Inline assembly** - Platform-specific optimizations
4. **Union field access** - Type punning scenarios
5. **Custom allocators** - Building your own memory management

## Key Concepts

### `unsafe` Blocks

Code inside an `unsafe` block bypasses Zith's safety checks:

```zith
unsafe {
    // All code here bypasses safety checks
    let ptr: *mut u8 = cast_to_ptr(0x1000);
    *ptr = 42;  // Direct memory write
}
```

### Raw Pointers

Raw pointers (`*const T` and `*mut T`) give you direct memory access without compiler guarantees:

```zith
let value: i32 = 100;

// Immutable raw pointer
let imm_ptr: *const i32 = &value;

// Mutable raw pointer (requires unsafe)
let mut_ptr: *mut i32 = unsafe { addr_of_mut!(value) };
```

## When to Use Raw & Unsafe

✅ **Appropriate uses:**
- Writing FFI bindings for C libraries
- Implementing custom allocators or data structures
- Performance-critical code with proven safety
- Inline assembly for hardware access

❌ **Avoid when:**
- Safe Zith constructs would work
- You're not certain about memory invariants
- The code will be frequently modified
- Safety can be guaranteed at runtime instead

## Topics in This Section

| Topic | Description |
|-------|-------------|
| [How to Use](./how-to-use.md) | Best practices for organizing raw/unsafe code |
| [Traits](./traits.md) | Advanced trait usage with raw types |
| [Generics Deep Dive](./generics-deep.md) | Complex generic patterns |
| [Metaprogramming](./metaprogramming.md) | Code generation techniques |
| [Macros](./macros.md) | Template and macro systems |
| [Data Structures](./data-structures.md) | Building custom data structures |
| [Unsafe Operations](./unsafe.md) | Detailed unsafe block usage |
| [Raw Pointers](./raw-pointers.md) | Complete raw pointer guide |

## Safety Guidelines

1. **Minimize unsafe scope** - Keep `unsafe {}` blocks as small as possible
2. **Document invariants** - Explain why the unsafe code is safe
3. **Add runtime checks** - Validate inputs before entering unsafe blocks
4. **Write tests** - Especially for unsafe code paths
5. **Review carefully** - Unsafe code requires extra scrutiny

### Example: Safe Wrapper Around Unsafe Code

```zith
// Good: Small unsafe block with validation
fn get_element(arr: &[i32], index: usize): Option<i32> {
    if index >= arr.len() {
        return None;
    }
    
    unsafe {
        Some(*arr.as_ptr().add(index))
    }
}
```

## Next Steps

- Learn [how to use](./how-to-use.md) raw and unsafe features responsibly
- Dive into [raw pointers](./raw-pointers.md) for memory manipulation
- Understand [unsafe operations](./unsafe.md) in detail

---

:::caution Remember

With great power comes great responsibility. Always prefer safe Zith constructs when possible.

:::
