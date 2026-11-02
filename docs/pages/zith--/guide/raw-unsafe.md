---
id: guide-raw-unsafe
title: Raw & Unsafe
section: Language Guide
output: zith--/guide/D-raw-unsafe.html
aliases: language/D-raw-unsafe.html
kind: editorial
---
# Raw & Unsafe

`raw` makes one unchecked operation explicit. Use it at a small, audited
boundary where the program has a guarantee that the compiler cannot prove.
The current compiler implements several `raw` expressions and `raw fn`.
Zith defines a separate `unsafe` hierarchy; it is not part of the Zith--
programming model.

## Extract an optional value

An optional value must normally be checked before its payload is used:

```zith
fn valueOrZero(value: ?i32): i32 {
    if (value) {
        return raw value;
    }

    return 0;
}
```

After an explicit check, `raw` can extract the payload without another runtime
check:

```zith
fn valueAfterCheck(value: ?i32): i32 {
    if (value is null) {
        return 0;
    }

    return raw value;
}
```

`raw value` does not make `null` valid. If the condition is wrong, the result is
invalid. The caller has taken responsibility for the guarantee.

## Read an intentionally uninitialized binding

The normal language rejects a read before the first assignment:

```zith
fn invalidRead(): i32 {
    let value: i32;
    return value;
}
```

`raw` makes the escape explicit:

```zith
fn externalStorage(): i32 {
    let value: i32;
    return raw value;
}
```

This does not initialize `value` or provide a default. Use it only when the
storage has been initialized by an external mechanism that the compiler cannot
see.

## Bypass a known union or opaque check

Tagged values use a checked cast when the code does not know which member is
present:

```zith
fn asInteger(value: opaque): ?i32 {
    value as i32
}
```

The unchecked form is appropriate only after a separate invariant has been
established:

```zith
fn knownInteger(value: raw opaque): i32 {
    raw value as i32
}
```

The same pattern applies to a union member:

```zith
fn knownMember(value: Any<i32, f64>): i32 {
    raw value as i32
}
```

The cast does not inspect the stored type. A wrong assumption produces an
invalid result.

## Use raw access for slices and pointers

`raw` is also useful when a low-level API needs an explicit unchecked access:

```zith
fn first(values: [3]i32): i32 {
    raw values[0]
}

fn asSlice(values: [3]i32): []i32 {
    raw values[0..3]
}
```

`raw` does not repair an invalid index or make a dangling pointer valid. A
runtime trap can still occur when the operation cannot be executed.

## `raw fn` is a separate choice

An expression-level `raw` applies to one operation:

```zith
fn unchecked(value: ?i32): i32 {
    raw value
}
```

`raw fn` marks a whole function as a low-level boundary:

```zith
raw fn readByte(buffer: *u8, index: u64): u8 {
    raw buffer[index]
}
```

Use `raw fn` for a small FFI or platform adapter. Keep validation in the safe
caller whenever possible, and keep the unchecked implementation isolated.

## When not to use it

Do not use `raw` only to silence a diagnostic. Prefer a checked branch when the
condition can be expressed normally. If the guarantee cannot be written down
in a comment or test, the operation is not ready to become raw.

`unsafe` blocks, raw unions, and the `Trust` capability are described in the
[Raw & Unsafe reference](doc:reference-13-raw-unsafe). They belong to Zith's
design and are outside the Zith-- language contract.
