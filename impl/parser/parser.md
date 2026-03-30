# Kalidous Parser Architecture Documentation

This document outlines the architecture of the Kalidous Parser, which has been modularized into four distinct files. The design follows a **Three-Pass Pipeline** strategy (Scan, Expand, Parse) to handle declarations, macros, and full body parsing efficiently.

## File Structure Overview

```text
impl/parser/
├── parser.cpp         # Entry Point & Pipeline Orchestration
├── parser_utils.cpp   # Low-level Utilities, Token Navigation, Error Handling
├── parser_expr.cpp    # Expression Parsing (Pratt Parser) & Types
└── parser_decl.cpp    # High-level Declarations, Statements & Body Logic
```

---

## 1. `parser.cpp`
**The Conductor**

This file acts as the entry point for the parsing system. It does not contain logic for specific language constructs (like `if` or `+`). Instead, it is responsible for the lifecycle of the parser: initialization, managing the three parsing modes, and resetting the state between passes.

### Responsibilities
*   **Initialization:** Sets up the `Parser` struct, configures the memory arena, and loads the source tokens.
*   **Pipeline Orchestration:** Implements the `kalidous_parse_with_source` function which drives the three phases:
    1.  **Scan Phase:** Runs the parser to collect signatures (functions, structs) while skipping bodies.
    2.  **Expand Phase:** A placeholder hook for future macro expansion or symbol table operations.
    3.  **Parse Phase:** Resets the token stream and parses the full AST, including all bodies.
*   **Top-Level Loop:** Manages the loop that calls `parser_parse_declaration` until the end of the file is reached.

### Key Functions
*   `parser_init(Parser*, ...)`: Initializes the parser state.
*   `kalidous_parse_with_source(...)`: The main public API called by the compiler driver.
*   `run_parser_phase(Parser*, KalidousParserMode)`: Internal helper to execute a specific mode (Scan/Parse).

---

## 2. `parser_utils.cpp`
**The Toolbox**

This file contains the fundamental building blocks used by all other parser files. It handles the interaction with the Lexer (Token Stream) and manages error reporting. No semantic logic regarding the language grammar lives here.

### Responsibilities
*   **Token Navigation:** Provides functions to inspect (`peek`), consume (`advance`), and validate (`expect`) tokens.
*   **Error Management:** Handles the `DiagList` (diagnostics buffer). It formats errors, emits warnings, and prevents cascading error messages.
*   **Recovery:** Implements **Panic Mode Synchronization**. If the parser encounters a syntax error, this logic helps it find a safe "sync point" (like a semicolon or closing brace) to continue parsing.
*   **Skipping:** Provides `skip_block`, which is critical for the **Scan Mode**. It blindly consumes tokens until a block is closed without parsing them, allowing the Scan phase to run very fast.

### Key Functions
*   `parser_peek(Parser*)`, `parser_advance(Parser*)`: Token stream accessors.
*   `parser_match(Parser*, TokenType)`: Checks and consumes a token if it matches.
*   `parser_expect(Parser*, TokenType, msg)`: Consumes a token or emits an error if missing.
*   `parser_error(Parser*, ...)`: Emits a diagnostic and sets the panic flag.
*   `parser_synchronize(Parser*)`: Resumes parsing after an error.
*   `skip_block(Parser*)`: Used during Scan mode to ignore function bodies.
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
*   **Statements:** Parses control flow structures (`if`, `for`, `while`, `return`) and blocks `{ ... }`.
*   **Function/Struct Internals:** Parses parameter lists, function bodies, and struct fields.
*   **Mode Logic:** This file contains the logic that decides **what to do** based on the current `Parser->mode`. It checks if we are in `SCAN` mode and calls `skip_block` (from utils), or if we are in `PARSE` mode and calls `parse_body` to recursively parse statements.

### Key Functions
*   `parser_parse_declaration(Parser*)`: The main loop for the top level. Dispatches to specific parsers (fn, struct, etc.).
*   `parser_parse_statement(Parser*)`: The main loop for inside functions/blocks. Dispatches to `if`, `return`, etc.
*   `parse_fn_decl(...)`: Parses function signatures and decides whether to skip or parse the body.
*   `parse_struct_decl(...)`: Parses struct definitions, including visibility modifiers and fields.
*   `parse_body(Parser*)`: Handles single-statement bodies vs. block bodies `{ ... }`.

---

## Data Flow

1.  **Entry:** `parser.cpp` initializes the `Parser` struct.
2.  **Scan Phase:**
    *   `parser.cpp` calls `parser_parse_declaration` (defined in `parser_decl.cpp`).
    *   `parser_decl.cpp` parses a `fn` signature using `parser_parse_type` (from `parser_expr.cpp`) and `parser_expect` (from `parser_utils.cpp`).
    *   When the body `{ ... }` is reached, `parser_decl.cpp` calls `skip_block` (from `parser_utils.cpp`) to ignore it.
3.  **Parse Phase:**
    *   `parser.cpp` resets the token position.
    *   `parser_decl.cpp` encounters the same `fn`.
    *   Instead of skipping, it calls `parser_parse_statement` recursively.
    *   `parser_parse_statement` calls `parser_parse_expression` (from `parser_expr.cpp`) to handle logic inside the function.
    *   If an error occurs, `parser_error` (from `parser_utils.cpp`) is called, and `parser_synchronize` attempts recovery.