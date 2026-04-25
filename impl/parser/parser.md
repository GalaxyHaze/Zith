# Zith Parser Architecture Documentation

This document outlines the architecture of the Zith Parser, which has been modularized into six distinct files. The design follows a **Three-Pass Pipeline** strategy (Scan, Expand, Sema) to handle declarations, macro expansion, and semantic analysis efficiently.

## File Structure Overview

```text
impl/parser/
├── parser.cpp         # Entry Point & Pipeline Orchestration
├── parser_utils.cpp   # Low-level Utilities, Token Navigation, Error Handling
├── parser_expr.cpp    # Expression Parsing (Pratt Parser) & Types
├── parser_decl.cpp    # High-level Declarations, Statements & Body Logic
├── parser_sema.cpp    # Semantic Analysis Phase
└── parser_test.cpp    # Test Utilities & RAII Wrappers
```

---

## 1. `parser.cpp`
**The Conductor**

This file acts as the entry point for the parsing system. It does not contain logic for specific language constructs (like `if` or `+`). Instead, it is responsible for the lifecycle of the parser: initialization, managing the three parsing modes, and resetting the state between passes.

### Responsibilities
*   **Initialization:** Sets up the `Parser` struct, configures the memory arena, and loads the source tokens.
*   **Pipeline Orchestration:** Implements the `zith_parse_with_source` function which drives the three phases:
    1.  **SCAN Phase:** Runs the parser to collect signatures (functions, structs) while capturing function bodies as UNBODY nodes (raw token streams). No type information is needed at this stage.
    2.  **EXPAND Phase:** Walks the tree and replaces UNBODY nodes with fully parsed BLOCK nodes. This is where statement-level parsing happens inside function bodies.
    3.  **SEMA Phase:** Semantic analysis — name resolution, type checking, borrow checker, control-flow analysis. Produces an annotated AST.
*   **Top-Level Loop:** Manages the loop that calls `parser_parse_declaration` until the end of the file is reached.

### Key Functions
*   `parser_init(Parser*, ...)`: Initializes the parser state.
*   `zith_parse_with_source(...)`: The main public API called by the compiler driver.
*   `run_parser_phase(Parser*, ZithParserMode)`: Internal helper to execute a specific mode (Scan/Expand/Sema).
*   `expand_unbody(...)`: Replaces UNBODY nodes with fully parsed BLOCK nodes.

---

## 2. `parser_utils.cpp`
**The Toolbox**

This file contains the fundamental building blocks used by all other parser files. It handles the interaction with the Lexer (Token Stream) and manages error reporting. No semantic logic regarding the language grammar lives here.

### Responsibilities
*   **Token Navigation:** Provides functions to inspect (`peek`), consume (`advance`), and validate (`expect`) tokens.
*   **Error Management:** Handles the `DiagList` (diagnostics buffer). It formats errors, emits warnings, and prevents cascading error messages.
*   **Recovery:** Implements **Panic Mode Synchronization**. If the parser encounters a syntax error, this logic helps it find a safe "sync point" (like a semicolon or closing brace) to continue parsing.
*   **Skipping:** Provides `skip_block`, which blindly consumes tokens until a block is closed. Used in error recovery and struct body parsing.

### Key Functions
*   `parser_peek(Parser*)`, `parser_advance(Parser*)`: Token stream accessors.
*   `parser_match(Parser*, TokenType)`: Checks and consumes a token if it matches.
*   `parser_expect(Parser*, TokenType, msg)`: Consumes a token or emits an error if missing.
*   `parser_error(Parser*, ...)`: Emits a diagnostic and sets the panic flag.
*   `parser_synchronize(Parser*)`: Resumes parsing after an error.
*   `skip_block(Parser*)`: Used for error recovery and struct body parsing.
*   `parse_lit_number(...)`: Converts raw token strings into integer/float values.

---

## 3. `parser_expr.cpp`
**The Calculator**

This file is responsible for the "middle layer" of the grammar: Types and Expressions. It implements a **Pratt Parser** (Top-Down Operator Precedence) to handle mathematical and logical expressions with correct precedence.

### Responsibilities
*   **Expression Parsing:** Parses literals, identifiers, unary operators (`-x`), binary operators (`a + b`), and function calls.
*   **Operator Precedence:** Determines that `a + b * c` is parsed as `a + (b * c)`.
*   **Type Parsing:** Handles complex type definitions, including pointers (`*T`), arrays (`[T]`), and optional/result types (`T?`, `T!`).
*   **Member Access:** Handles field access (`obj.field`) and method calls (`obj.method()`).

### Key Functions
*   `parser_parse_expression(Parser*)`: Entry point for parsing any expression.
*   `parser_parse_type(Parser*)`: Entry point for parsing a type signature.
*   `parse_expr_bp(Parser*, int)`: The recursive core of the Pratt parser.
*   `parse_nud(Parser*)`: "Null Denotation" - handles prefixes (literals, unary ops).
*   `infix_bp(TokenType)`: Defines the binding power (precedence) for operators.

---

## 4. `parser_decl.cpp`
**The Structure**

This is the largest and most complex file. It handles the "high level" grammar: Declarations (top-level items) and Statements (inside functions). It dictates the overall shape of the Abstract Syntax Tree (AST).

### Responsibilities
*   **Top-Level Declarations:** Parses `fn`, `struct`, `enum`, `import`, and global variable declarations.
*   **Statements:** Parses control flow structures (`if`, `for`, `return`) and blocks `{ ... }`.
*   **Function/Struct Internals:** Parses parameter lists, function bodies, and struct fields.
*   **Mode Logic:** This file contains the logic that decides **what to do** based on the current `Parser->mode`. In `SCAN` mode, function bodies are captured as UNBODY via `capture_unbody`. In `EXPAND` mode (TODO), UNBODY nodes will be replaced with fully parsed BLOCKs.
*   **Symbol Collection:** In SCAN mode, the `ScanSymbolCollector` singleton tracks all declared symbols (functions, structs, traits, enums, imports) for later symbol table construction and debugging.

### Key Functions
*   `parser_parse_declaration(Parser*)`: The main loop for the top level. Dispatches to specific parsers (fn, struct, etc.).
*   `parser_parse_statement(Parser*)`: The main loop for inside functions/blocks. Dispatches to `if`, `return`, etc.
*   `parse_fn_decl(...)`: Parses function signatures and decides whether to capture the body as UNBODY (SCAN) or parse it fully (EXPAND — TODO).
*   `parse_struct_decl(...)`: Parses struct definitions, including visibility modifiers and fields.
*   `parse_body(Parser*)`: Handles single-statement bodies vs. block bodies `{ ... }`.
*   `capture_unbody(...)`: Captures raw tokens between `{` and `}` as an UNBODY node (SCAN mode only).
*   `ScanSymbolCollector` (class): Singleton that collects all scanned symbols during SCAN mode. Supports `print_scanned_symbols()` for debugging. Symbol kinds: `fn`, `struct`, `trait`, `enum`, `import`.

---

## 5. `parser_sema.cpp`
**The Analyzer**

This file handles the third phase of the pipeline: Semantic Analysis. It performs type checking, name resolution, scope management, and validates language semantics.

### Responsibilities
*   **Type Inference:** Determines the type of expressions based on literals and context.
*   **Type Checking:** Validates type compatibility in assignments, return statements, and binary operations.
*   **Name Resolution:** Resolves identifiers to their declared definitions, reporting undefined references.
*   **Scope Management:** Tracks variable definitions across nested scopes (blocks, functions, control flow).
*   **Return Validation:** Ensures return types match the function's declared return type.

### Key Functions
*   `sema_run(Parser*, ZithNode*)`: Entry point for semantic analysis. Walks the entire AST.
*   `sema_expr(...)`: Analyzes expressions, returning type information.
*   `sema_stmt(...)`: Analyzes statements, validating type usage.
*   `sema_type_from_node(...)`: Converts AST type nodes to semantic type information.
*   `sema_lookup(...)`: Looks up variable/type in current and enclosing scopes.
*   `sema_define(...)`: Registers a new variable/type in the current scope.

### Type System
*   Base types: `void`, `int`, `float`, `string`, `bool`, `module`
*   Type qualifiers: `optional` (`?`), `failable` (`!`)

---

## 6. `parser_test.cpp`
**The Test Harness**

This file provides convenience utilities for writing tests. It wraps the parser in a simple API and manages a shared memory arena.

### Responsibilities
*   **Test API:** Simple functions to parse source strings without manual arena management.
*   **Global Arena:** Shared arena that's reset between test cases.
*   **RAII Wrapper:** C++ `ParseResult` class that automatically cleans up.

### Key Functions
*   `zith_parse_test(const char*)`: Parse source in SCAN mode only.
*   `zith_parse_test_full(const char*)`: Full pipeline (Scan + Expand + Sema).
*   `zith_test_arena_destroy(void)`: Cleanup global arena.
*   `ParseResult` (class): RAII wrapper for test results.

---

## Data Flow

1.  **Entry:** `parser.cpp` initializes the `Parser` struct.
2.  **SCAN Phase:**
    *   `parser.cpp` calls `parser_parse_declaration` (defined in `parser_decl.cpp`).
    *   `parser_decl.cpp` parses a `fn` signature using `parser_parse_type` (from `parser_expr.cpp`) and `parser_expect` (from `parser_utils.cpp`).
    *   When the body `{ ... }` is reached, `parser_decl.cpp` calls `capture_unbody` to store the raw token stream as an UNBODY node — no parsing of the body content happens here.
    *   Structs, imports, and top-level expressions are fully parsed.
    *   Each symbol (fn, struct, import, etc.) is registered with `ScanSymbolCollector`.
3.  **Symbol Printing:**
    *   After SCAN phase completes, `print_scanned_symbols()` outputs all collected symbols to stdout.
4.  **EXPAND Phase:**
    *   `parser.cpp` calls `expand_unbody` to walk the AST.
    *   For each UNBODY node, creates a sub-parser and fully parses the token stream into a BLOCK node.
    *   This is where `parser_parse_statement` and `parser_parse_expression` are called recursively on the captured bodies.
5.  **SEMA Phase:**
    *   `parser.cpp` calls `sema_run` (from `parser_sema.cpp`).
    *   Traverses the fully parsed AST for semantic analysis.
    *   Name resolution, type checking, control-flow analysis.
    *   Produces an annotated AST ready for code generation.

---

## Semantic Analysis Coverage

This section documents what the `parser_sema.cpp` module currently checks and validates.

### Type System

**Supported Base Types:**
- `void` - absence of value
- `int`, `i8`, `i16`, `i32`, `i64` - signed integers
- `u8`, `u16`, `u32`, `u64` - unsigned integers
- `float`, `f32`, `f64` - floating point
- `str`, `string` - string literals
- `bool` - boolean
- `module` - imported module references

**Type Qualifiers:**
- `?` (optional) - value may be null
- `!` (failable) - value may be an error

### Checks Performed

**1. Name Resolution**
- [x] Undefined identifier detection
- [x] Undefined function call detection
- [x] Scope-based variable lookup (nested scopes)

**2. Type Checking**
- [x] Variable declaration type matching
- [x] Assignment type compatibility
- [x] Optional type assignability (`?` can assign to non-optional)
- [x] Failable type assignability (`!` can assign to non-failable)

**3. Return Statements**
- [x] Void function cannot return value
- [x] Return type must match function declaration
- [x] Type mismatch error messages with expected/got types

**4. Binary Operations**
- [x] String concatenation (`+` with two strings)
- [x] String + other type = error
- [x] Int + Int = Int
- [x] Float + any numeric = Float
- [x] Comparison operators return Bool (`==`, `!=`, `<`, `<=`, `>`, `>=`, `&&`, `||`)
- [x] Arithmetic operators return Int (`+`, `-`, `*`, `/`, `%`)

**5. Literals**
- [x] Integer literals → `int`
- [x] Unsigned literals → `int` (treated as unsigned during parsing)
- [x] Float literals → `float`
- [x] String literals → `string`
- [x] Boolean literals → `bool`

**6. Built-in Functions**
- [x] `print(...)` - validated but returns `void`
- [x] `println(...)` - validated but returns `void`

**7. Control Flow**
- [x] If statement - evaluates condition
- [x] For loop - creates new scope, evaluates all components
- [x] Block - creates new scope for variable isolation

**8. Scope Management**
- [x] Variable definitions registered in scope
- [x] Function parameters registered in scope
- [x] Import aliases registered as module type
- [x] Nested scopes (blocks, loops) properly isolated
- [x] Shadowing allowed (inner scope overrides outer)

### Not Yet Implemented

- [ ] Struct field access validation
- [ ] Method call validation
- [ ] Import path resolution
- [ ] Control flow analysis (unreachable code, missing returns)
- [ ] Borrow checking (ownership)
- [ ] Constant evaluation
- [ ] Generic type parameters
- [ ] Interface/trait constraints