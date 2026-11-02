---
id: getting-started-quick-start
title: First Program
section: Zith--
output: zith--/guide/D-first-program.html
aliases: D-quickstart.html
kind: editorial
---
# First Program

Zith-- is in beta. This guide uses the working `zithc create`, `build`, and
`run` commands described in [Implementation Status](doc:reference-implementation-status).

## Create a project

```bash
zithc create hello-zith
cd hello-zith
```

`create` writes a `ZithProject.toml` and `src/main.zith` scaffold.

## Write a program

Replace `src/main.zith` with a small program:

```zith
fn add(a: i32, b: i32): i32 { a + b }

fn main() {
    let total = add(20, 22);
}
```

## Build and run

```bash
zithc check
zithc build
zithc run
```

`check` type-checks without emitting output. `run` compiles and executes in one step. Continue with the [Introduction](doc:getting-started-introduction) for the documentation map, or read [Syntax](doc:guide-syntax) before expanding the program.
