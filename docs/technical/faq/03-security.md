---
id: security
title: Security Model
sidebar_label: Security
description: How Zith ensures memory safety and prevents common vulnerabilities.
---

# Security Model

Zith provides strong safety guarantees without runtime overhead through its compile-time ownership system.

## Memory Safety Guarantees

### What Zith Prevents at Compile-Time

| Vulnerability | How Zith Prevents It |
|--------------|---------------------|
| **Use-after-free** | Ownership tracking ensures memory is valid |
| **Double-free** | Unique ownership prevents multiple frees |
| **Data races** | Mutability rules prevent concurrent writes |
| **Buffer overflows** | Bounds checking (optional in release) |
| **Null pointer dereference** | No null by default, use `Option` types |
| **Uninitialized memory** | Compiler enforces initialization |

## Ownership System

### Three Ownership Modifiers

```zith
// unique - Full ownership, only one owner
let data: unique i32 = alloc.new(42);

// view - Read-only borrow, multiple allowed
let ref1: view i32 = data;
let ref2: view i32 = data;

// mut - Mutable borrow, exclusive access
let mut_ref: mut i32 = data;  // Transfers ownership temporarily
```

### Example: Safe Memory Access

```zith
fn process_data() {
    // Allocate with explicit ownership
    let buffer: unique [u8] = alloc.new([0; 1024]);
    
    // Create read-only views (safe, multiple allowed)
    let header: view [u8] = buffer[0..64];
    let payload: view [u8] = buffer[64..];
    
    // Can read from both views
    print("Header: {header}");
    print("Payload: {payload}");
    
    // buffer still owns the memory, freed at end of scope
}  // Automatic cleanup here
```

## Comparison with Other Languages

### vs C/C++

```c
// C - Manual, error-prone
void process() {
    int* ptr = malloc(sizeof(int) * 100);
    // ... use ptr ...
    free(ptr);
    free(ptr);  // ❌ Double-free! Undefined behavior
}
```

```zith
// Zith - Compile-time safety
fn process() {
    let ptr: unique [i32] = alloc.new([0; 100]);
    // ... use ptr ...
}  // ✅ Automatically freed, double-free impossible
```

### vs Rust

```rust
// Rust - Powerful but complex lifetimes
fn process<'a>(data: &'a mut Vec<i32>, index: usize) -> &'a i32 {
    &data[index]  // Lifetime annotations required
}
```

```zith
// Zith - Simpler, implicit lifetimes
fn process(data: mut [i32], index: usize): view i32 {
    return data[index];  // ✅ No lifetime annotations needed
}
```

## Unsafe Code

When you need to bypass safety checks:

```zith
unsafe {
    // Raw pointer operations
    let raw_ptr = alloc.raw_alloc(1024);
    
    // FFI calls
    libc::printf("Unsafe operation\n");
    
    // Inline assembly
    asm!("nop");
}  // You're responsible for safety here
```

**Best Practice:** Minimize unsafe blocks, isolate them, and document why they're safe.

## Security Best Practices

### 1. Use Safe Abstractions

```zith
// ✅ Good: Use safe collections
let vec = Vec<i32>.new();

// ❌ Avoid: Raw pointers when not needed
let ptr = alloc.raw_alloc(...);
```

### 2. Leverage the Type System

```zith
// Encode invariants in types
struct NonZeroU32 {
    value: u32,  // Invariant: never zero
    
    fn new(v: u32): Option<Self> {
        if v == 0 { return None; }
        return Some(Self { value: v });
    }
}
```

### 3. Minimize Unsafe

```zith
// ✅ Isolate unsafe code
fn safe_wrapper() {
    unsafe {
        // Only the necessary unsafe operations
    }
    // Rest of function is safe
}
```

### 4. Use Views for Borrowing

```zith
// ✅ Pass views instead of copying
fn process_large_data(data: view [u8]) {
    // Read-only access, no copy
}

// ❌ Don't pass by value unnecessarily
fn process_large_data(data: [u8]) {  // Copies entire array!
```

## Auditing for Security

When reviewing Zith code:

1. **Check unsafe blocks** - Are they necessary? Documented?
2. **Verify ownership transfers** - No accidental moves?
3. **Review FFI boundaries** - Proper validation?
4. **Look for view mutations** - Using `view` where `mut` needed?

## Limitations

Zith's safety model doesn't prevent:

- **Logic errors** - Wrong algorithms are still possible
- **Integer overflows** - Enable overflow checks in debug
- **Resource leaks** - File handles, network connections (use RAII)
- **Side channels** - Timing attacks, cache attacks

## Conclusion

Zith provides **strong memory safety guarantees** through its ownership system, preventing entire classes of vulnerabilities at compile-time while maintaining zero runtime overhead.

---

**Next:** Compare with [Rust](./rust-comparison.md) →
