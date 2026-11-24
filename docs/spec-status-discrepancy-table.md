# Zith-- Spec, Status, And Compiler Discrepancy Table

Date: 2026-11-24
Revision: compiler `Zith` HEAD `29444d4e` (build tree `build/`), website
`Zith-website` HEAD `3097311`.
Scope: every row in `docs/impl-status.md`, plus every public-claim finding in
`docs/review-website-inconsistencies-2026-11-02.md` and the website
source-file references rechecked for this beta.

This table is the Step 2 deliverable "one row per status or public-claim
issue". It records, for each feature or claim, the specification section, the
`docs/impl-status.md` entry, the compiler evidence, the discrepancy found, and
the decision or resolution. It is a log of the 2026-11-24 audit, not a second
source of truth: `docs/impl-status.md` remains the status authority and
`docs/Zith--.md` remains the language contract.

## Method

- The status authority is `docs/impl-status.md`. The language contract is
  `docs/Zith--.md`. Implementation rules are in
  `docs/Zith---implementation.md`. `docs/Zith-spec.md` is the separate Zith
  design and was consulted only to catch accidental conflation.
- Evidence is one of: a test id `tests/<file>.cpp::<test_name>` from the full
  `ctest --test-dir build` run (51 of 51 passed on 2026-11-24), a `zithc`
  command with its observed result, or a source reference.
- Every `Working` entry used in beta material carries at least one passing
  test or a reproducible compiler example, per Step 2 Action 4.
- Discrepancy values: `none` when the compiler matches the status, or a short
  description when they differ. Decision values: `keep` when the row is
  accurate as written, `fix` when the row, the website, or the compiler was
  changed, and `defer` when the difference is a recorded limitation.

## Evidence Base Rechecked On 2026-11-24

- `cmake --build build -j4`: exit 0, no work to do (tree already built).
- `ctest --test-dir build --output-on-failure -j4`: 51 of 51 passed in 7.94 s.
- `zithc check` over `examples/*.zithm`: 50 of 55 pass. The five failures are
  the exploratory `examples/test-import-{debug,debug2,debug3,full,full-fn}.zithm`
  probes, which import `soon/counter` and are not in the `test-examples`
  manifest. `soon/` is experimental and outside the beta surface, so this is
  not a beta discrepancy.
- `zithc check examples/loops-simple.zithm`: exit 0.
- `zithc create` scaffolds `src/main.zithm` and writes
  `entry = "src/main.zithm"` (`src/cli/cmd/project.cpp`).

## Compiler Pipeline

| Feature | Spec section | Status entry | Compiler evidence | Discrepancy | Resolution |
|---|---|---|---|---|---|
| Lexer | `Zith--.md` (no section, see `Zith---implementation.md`) | Working | `tests/test-frontend.cpp::test_multi_char_operators_are_single_tokens`, `tests/test-frontend.cpp::test_dots_lex_as_one_token`, `tests/test-op-mapping.cpp` | none | keep |
| Parser | `Zith--.md` | Working | `tests/test-frontend.cpp` (64 cases), `tests/test-frontend-modern-pipeline.cpp` | none | keep |
| Formatter | `Zith--.md` | Working | `tests/test-formatter.cpp` (30 round-trip cases) | none | keep |
| Import resolution | `Zith--.md` "Imports Platform-Specific" | Working | `tests/test-frontend-context.cpp::test_platform_import_resolution_order`, `::test_platform_import_arch_only`, `::test_platform_import_os_only`, `::test_platform_import_generic_fallback`, `tests/test-frontend-modern-pipeline.cpp::test_pipeline_export_platform_import` | none | keep |
| Name resolution | `Zith--.md` "Bindings" | Working | `tests/test-frontend-context.cpp::test_local_bindings_are_scoped`, `::test_lookup_binding_walks_module_fallback`, `::test_parameter_shadowing_in_body_is_a_duplicate` | none | keep |
| Type checking | `Zith--.md` | Working | `tests/test-frontend-modern-pipeline.cpp`, `tests/test-modern-types.cpp`, `tests/test-parse-input.cpp` | none | keep |
| Generic instantiation | `Zith--.md` "Inferência genérica com `?T`" | Working | `tests/test-generics-mono.cpp`, `tests/test-generic-hashmap.cpp`, `tests/test-enum-union-generics.cpp`, `tests/test-generic-constraints.cpp` | none | keep |
| Comptime / Solve | `Zith--.md` "Fora do Escopo Atual de Zith--" | Reserved | source: the solver is a compatibility stub (`src/comptime/`) and macro expansion runs in the frontend | none | keep |
| SRA / Reference Analysis | `Zith--.md` "Ownership" | Working (frozen) | `tests/test-memory-qualifiers.cpp`, `tests/test-hir-lower-modern.cpp::test_ownership_hir_carries_residual_slot_attrs` | none | keep |
| HIR lowering | `Zith---implementation.md` | Working | `tests/test-hir-lower-modern.cpp` (95 cases) | none | keep |
| LLVM codegen | `Zith--.md` | Working | `tests/test-codegen.cpp` (199 cases), `tests/test-examples.cpp` | none | keep |
| Cache | `Zith--.md` | Working | `tests/test-cache.cpp` (20 cases), `tests/test-zirl-sections.cpp`, `tests/test-cache-entry.cpp` | none | keep |
| Stdlib I/O | `Zith--.md` "Superfície C e Biblioteca" | Working | `tests/test-stdlib-io.cpp`, `tests/test-codegen.cpp::test_import_stdio_runs` | none | keep |
| Stdlib allocation | `Zith--.md` | Working | `tests/test-codegen.cpp::test_imported_memory_new_delete_runtime`, `::test_imported_memory_make_release_runtime`, `tests/test-interface-satisfaction.cpp` | none | keep |
| Stdlib collections | `Zith--.md` | Working | `tests/test-generic-hashmap.cpp`, `tests/test-codegen.cpp::test_imported_generic_hashmap_runtime`, `::test_imported_generic_dynarray_runtime` | none | keep |
| Stdlib error handling | `Zith--.md` | Working | `tests/test-codegen.cpp::test_imported_result_runtime`, `::test_imported_panic_runtime`, `tests/test-stdlib-modules.cpp` | none | keep |

## Language Features: Functions And Bindings

| Feature | Spec section | Status entry | Compiler evidence | Discrepancy | Resolution |
|---|---|---|---|---|---|
| `fn` | `Zith--.md` "Bindings" | Working | `tests/test-frontend.cpp::test_function_body_ast`, `tests/test-codegen.cpp::test_function_default_arguments_runtime` | none | keep |
| `[a, b]` binding destructuring | `Zith--.md` "Bindings" | Partial | `tests/test-abi-execution.cpp` (pack broadcast + destructuring) | none | keep (limitation preserved in public copy) |
| Call-result statements | `Zith--.md` | Working | `tests/test-frontend-modern-pipeline.cpp::test_pipeline_discard_non_void_call_result`, `::test_pipeline_discardable_attribute_allows_call` | none | keep |
| `#[test]`, `@assert`, `@check`, `@log` | `Zith--.md` "Diagnósticos e warnings" | Working | `tests/test-zith-test-suite.cpp`, `tests/test-cli-commands.cpp::test_test_command_filters_and_executes_tests` | none | keep |
| generic parameter lists `<T, U>` | `Zith--.md` "Inferência genérica com `?T`" | Working (step 04/05) | `tests/test-generics-mono.cpp`, `tests/test-generic-constraints.cpp` | none | keep |
| `state` machine | `Zith--.md` "Controlo de Fluxo" | Working | `tests/test-codegen.cpp::test_state_machine_loop_executes`, `::test_state_tail_calls_emit_musttail`, `::test_state_machine_diverging_parameters_executes` | none | keep |
| Function return termination | `Zith--.md` | Working | `tests/test-frontend-modern-pipeline.cpp::test_inferred_return_type_rejects_incompatible_returns` | none | keep |
| Inferred function return type | `Zith--.md` | Working | `tests/test-frontend-modern-pipeline.cpp::test_inferred_return_type_from_value_tail`, `::test_inferred_return_type_rejects_indirect_recursion` | none | keep |
| `raw fn` | `Zith--.md` "`raw fn`" | Working | `tests/test-frontend-modern-pipeline.cpp::test_raw_call_markers_cover_direct_and_method_invocations`, `::test_raw_functions_are_not_function_values` | none | keep |
| `const fn` | `Zith--.md` "Restrições Removidas" | Parse error | `zithc check` on `const fn f(): i32 { 1 }` reports `E2010 'const fn' is not supported` | none | keep |
| `extern fn` | `Zith--.md` "Superfície C e Biblioteca" | Working | `tests/test-hir-lower-modern.cpp::test_extern_and_main_lower_to_hir`, `tests/test-codegen.cpp::test_c_scalar_arguments_and_results_run` | none | keep |
| `let`, `var`, `const` | `Zith--.md` "Bindings", "Globals" | Working | `tests/test-frontend.cpp::test_zith_removed_bindings_and_qualifiers`, `tests/test-codegen.cpp::test_const_global_runtime` | none | keep |
| `#[alignas(N)]` on bindings | `Zith--.md` "Alinhamento explícito" | Working | `tests/test-codegen.cpp::test_explicit_array_binding_alignment`, `::test_explicit_global_binding_alignment` | none | keep |
| `#[noalias]`, `#[likely]`, `#[unlikely]` | `Zith--.md` "Diagnósticos e warnings" | Recognized only | `tests/test-parse-input.cpp::test_noalias_requires_pointer_return`, `tests/test-frontend.cpp::test_attributes_parse_and_classify` | none | keep (limitation preserved) |

## Language Features: Types

| Feature | Spec section | Status entry | Compiler evidence | Discrepancy | Resolution |
|---|---|---|---|---|---|
| `bool`, `char` | `Zith--.md` "Tipos" | Working | `tests/test-examples.cpp` (`booleans-*`), `tests/test-codegen.cpp::test_c_bool_char_long_scalars_run` | none | keep |
| `i8`–`i128`, `u8`–`u128` | `Zith--.md` "Tipos" | Working | `tests/test-op-mapping.cpp::test_width_mapping`, `tests/test-examples.cpp` (`bitwise-*`) | none | keep |
| `f32`, `f64` | `Zith--.md` "Tipos" | Working | `tests/test-codegen.cpp::test_f32_literal_stores_in_32_width` | none | keep |
| `?T` (legacy optional wrapper) | `Zith--.md` "Inferência genérica com `?T`" | Working | `tests/test-optional-slice.cpp`, `tests/test-codegen.cpp::test_generic_optional_coercion_runtime` | none | keep |
| `T!` wrapper / return annotation | `Zith--.md` "Restrições Removidas" | Spec only | `zithc check` on `fn f(): i32! { 1 }` reports `E2010 unexpected token` | none | keep (limitation preserved) |
| `*T` (pointer) | `Zith--.md` "Pointer Object e `&`" | Working | `tests/test-codegen.cpp::test_double_pointer_roundtrip`, `tests/test-optional-slice.cpp::test_nullable_pointer_narrowing` | none | keep |
| `raw opaque` | `Zith--.md` "Opaque tagged" | Working | `tests/test-codegen.cpp::test_bare_opaque_raw_and_mismatch_runtime` | none | keep |
| `opaque` | `Zith--.md` "Opaque tagged" | Working | `tests/test-codegen.cpp::test_bare_opaque_runtime`, `tests/test-hir-lower-modern.cpp::test_opaque_canonical_tags_persist_across_sessions` | none | keep |
| `[N]T` (array), `[]T` (slice) | `Zith--.md` "Tipos" | Working | `tests/test-codegen.cpp::test_array_to_slice_runtime`, `tests/test-optional-slice.cpp` | none | keep |
| `\|T, U\|` (pack) | `Zith--.md` "Bindings" | Working | `tests/test-hir-lower-modern.cpp::test_anonymous_pack_literal_lowers_to_hir` | none | keep |
| `...pack` (call argument spread) | `Zith--.md` "Expansão de Packs em Chamadas" | Partial | `tests/test-codegen.cpp::test_pack_spread_runtime`, `::test_generic_method_pack_spread_runtime` | none | keep (limitation preserved) |
| `[...]T` (variadic slice) | `Zith--.md` "Variadic Slices" | Working | `tests/test-variadic.cpp`, `tests/test-codegen.cpp::test_variadic_slice_state_runtime` | none | keep |
| `fn(...): R` (function value) | `Zith--.md` "Funções anónimas" | Working | `tests/test-codegen.cpp::test_function_pointer_call` | none | keep |
| `fn (params): R { body }` (function literal) | `Zith--.md` "Funções anónimas" | Working | `tests/test-codegen.cpp::test_anonymous_function_literal_runtime`, `::test_anonymous_function_capture_is_rejected` | none | keep |
| `dyn Trait` / `dyn Interface` | `Zith--.md` "Tipos" | Working (methods) | `tests/test-codegen.cpp::test_dyn_trait_method_dispatch_runtime`, `::test_dyn_interface_field_access_is_rejected` | none | keep |
| `struct`, `component`, `enum`, `union` | `Zith--.md` "Component", "Enums" | Working | `tests/test-parse-input.cpp::test_component_field_constraints`, `tests/test-enum-union-generics.cpp` | none | keep |
| `trait`, `interface` | `Zith--.md` "Tipos" | Working (conformance) | `tests/test-trait-conformance.cpp`, `tests/test-interface-satisfaction.cpp` | none | keep |
| `implement T as Trait {}` | `Zith--.md` "Tipos" | Working | `tests/test-trait-parser.cpp::test_implement_trait_name_checks`, `tests/test-trait-conformance.cpp::test_duplicate_implementation` | none | keep |
| `type` | `Zith--.md` "`type` wrapper e `alias`" | Working (Zith-- cast-based) | `tests/test-nominal-type-debt.cpp` | none | keep |
| `alias` | `Zith--.md` "`type` wrapper e `alias`" | Working | `tests/test-nominal-type-debt.cpp::test_alias_remains_transparent` | none | keep |
| memory qualifiers (`mut`, `lend`, `view`, `own`, `share`, `belong`) | `Zith--.md` "Ownership" | Working (lend/view slice) | `tests/test-memory-qualifiers.cpp` | none | keep |

## Language Features: Expressions

| Feature | Spec section | Status entry | Compiler evidence | Discrepancy | Resolution |
|---|---|---|---|---|---|
| literals | `Zith--.md` "Tipos" | Working | `tests/test-hir-lower-modern.cpp::test_radix_literals_lower_to_their_value`, `tests/test-codegen.cpp::test_dollar_escape_hatch_runtime` | none | keep |
| unary `-`, `not` | `Zith--.md` | Working | `tests/test-frontend.cpp::test_loop_labels_and_not_negation` | none | keep |
| unary `~` | `Zith--.md` | Working | `tests/test-op-mapping.cpp::test_unary_mapping` | none | keep |
| binary arithmetic/comparison | `Zith--.md` | Working | `tests/test-op-mapping.cpp::test_binary_mapping` | none | keep |
| bitwise `&.` `\|.` `^.` | `Zith--.md` | Working | `tests/test-formatter.cpp::test_formatter_bitwise_operator_round_trip`, `tests/test-examples.cpp` (`bitwise-*`) | none | keep |
| assignment `=` | `Zith--.md` | Working | `tests/test-hir-lower-modern.cpp::test_bindings_lower_to_slots` | none | keep |
| compound assignment | `Zith--.md` | Working | `tests/test-compound-assign.cpp`, `tests/test-codegen.cpp::test_compound_assign_runtime` | none | keep |
| `&&`, `\|\|` | `Zith--.md` "Restrições Removidas" | Parse error | `zithc check` on `true && false` reports `E2010 '&&' is not a Zith operator; use 'and'` | none | keep |
| `++`, `--` | `Zith--.md` "Restrições Removidas" | Parse error | `zithc check` on `x++` reports `E2010 use explicit assignment` | none | keep |
| field access `x.field` | `Zith--.md` "Visibilidade de campos" | Working | `tests/test-hir-lower-modern.cpp::test_dot_field_read_lowers_to_hir_field` | none | keep |
| dereference `*p` | `Zith--.md` "Pointer Object e `&`" | Working | `tests/test-hir-lower-modern.cpp::test_addrof_and_deref_lowers_to_hir_unary` | none | keep |
| address-of `&x` | `Zith--.md` "Pointer Object e `&`" | Working | `tests/test-hir-lower-modern.cpp::test_addrof_and_deref_lowers_to_hir_unary` | none | keep |
| `p->field` | `Zith--.md` "Pointer Object e `&`" | Working | `tests/test-hir-lower-modern.cpp::test_arrow_access_lowers_to_deref_then_field` | none | keep |
| `\|>` / `also` pipeline | `Zith--.md` "Controlo de Fluxo" | Working | `tests/test-frontend.cpp::test_pipe_operators_parse_as_chain`, `tests/test-hir-lower-modern.cpp::test_pipe_also_requires_view_parameters` | none | keep |
| index `a[i]` | `Zith--.md` "Intrinsics de slices" | Working | `tests/test-codegen.cpp::test_checked_index_runtime`, `tests/test-frontend.cpp::test_raw_index_slice_expression` | none | keep |
| `?` postfix propagation | `Zith--.md` "Inferência genérica com `?T`" | Working | `tests/test-optional-slice.cpp`, `tests/test-codegen.cpp::test_optional_condition_keyword_runtime` | none | keep |
| `must` / `raw` optional extraction | `Zith--.md` "Inferência genérica com `?T`" | Working | `tests/test-codegen.cpp::test_bare_opaque_raw_and_mismatch_runtime` | none | keep |
| `as` cast | `Zith--.md` "Tipos" | Working | `tests/test-hir-lower-modern.cpp::test_numeric_cast_lowers_to_hir_cast` | none | keep |
| `is null` | `Zith--.md` "Pointer Object e `&`" | Working | `tests/test-hir-lower-modern.cpp::test_is_null_on_pointer_optional_uses_niche`, `::test_is_null_on_value_optional_reads_tag` | none | keep |
| `is <type>` | `Zith--.md` "Enums" | Working (tagged unions + opaque) | `tests/test-codegen.cpp::test_imported_tagged_union_is_member_runtime`, `tests/test-frontend.cpp::test_is_type_expression` | none | keep |
| range literals | `Zith--.md` "`in` e ranges" | Working | `tests/test-optional-slice.cpp::test_range_boundaries_and_empty_ranges` | none | keep |
| `in` | `Zith--.md` "`in` e ranges" | Working | `tests/test-codegen.cpp::test_range_in_operator_and_when_runtime` | none | keep |
| struct literal | `Zith--.md` "Tipos" | Working | `tests/test-hir-lower-modern.cpp::test_struct_literal_lowers_to_hir` | none | keep |
| `@sizeOf`, `@offsetOf`, `@alignOf` | `Zith--.md` "Intrinsics de slices" | Working | `tests/test-codegen.cpp::test_layout_api_matches_llvm` | none | keep |
| `@canonicalType` | `Zith--.md` "Intrinsics de slices" | Working | `tests/test-hir-lower-modern.cpp::test_canonical_type_lowers_to_hir_node`, `tests/test-zirl-sections.cpp` | none | keep |

## Language Features: Control Flow

| Feature | Spec section | Status entry | Compiler evidence | Discrepancy | Resolution |
|---|---|---|---|---|---|
| `if` / `else` / `else (cond)` / `else if` | `Zith--.md` "Controlo de Fluxo" | Working | `tests/test-hir-lower-modern.cpp::test_if_else_lowers_to_branch_and_merge`, `tests/test-frontend.cpp::test_else_condition_forms` | none | keep |
| `while` | `Zith--.md` "Restrições Removidas" | Deprecated | `tests/test-frontend.cpp::test_while_is_deprecated` | none | keep (limitation preserved) |
| `break`, `continue` | `Zith--.md` "Controlo de Fluxo" | Working | `tests/test-codegen.cpp::test_labeled_loop_controls_runtime` | none | keep |
| `return` | `Zith--.md` "Controlo de Fluxo" | Working | `tests/test-codegen.cpp::test_for_loop_return_runtime` | none | keep |
| `for (cond) { }`, `for { }` | `Zith--.md` "Controlo de Fluxo" | Working | `tests/test-hir-lower-modern.cpp::test_for_condition_lowers_like_while` | none | keep |
| `for (init, cond, step) { }` | `Zith--.md` "Controlo de Fluxo" | Working | `tests/test-codegen.cpp::test_for_three_clause_runtime` | none | keep |
| `for (x in xs)` | `Zith--.md` "Controlo de Fluxo" | Working | `tests/test-codegen.cpp::test_for_in_runtime`, `tests/test-examples.cpp` (`loops-*`) | none | keep |
| `when` / `match` pattern match | `Zith--.md` "Controlo de Fluxo" | Working | `tests/test-codegen.cpp::test_range_in_operator_and_when_runtime`, `tests/test-frontend.cpp::test_when_guard_islands` | none | keep |
| `state` / `jump` | `Zith--.md` "Controlo de Fluxo" | Working | `tests/test-codegen.cpp::test_state_machine_loop_executes`, `::test_pack_spread_state_dock_and_jump_runtime` | none | keep |
| `dock` | `Zith--.md` "Controlo de Fluxo" | Working | `tests/test-codegen.cpp::test_state_value_dock_runtime` | none | keep |
| `defer expr;` scope guards | `Zith--.md` "Controlo de Fluxo" | Working | `tests/test-codegen.cpp::test_defer_reverse_order_before_return`, `::test_defer_in_state_before_jump` | none | keep |
| named blocks and scope markers | `Zith--.md` "Blocos Nomeados e Marcadores de Scope" | Working | `tests/test-frontend-modern-pipeline.cpp::test_named_block_marker_accepts_each_place_kind`, `tests/test-codegen.cpp::test_named_block_scope_marker_cleans_up_on_early_return` | none | keep |
| `drop` cleanup hooks | `Zith--.md` "Fora do Escopo Atual de Zith--" | Rejected for Zith-- | `zithc check` on `drop x;` reports `E2001 unknown identifier 'drop'` | none | keep |

## Language Features: Words, Contexts, Macros

| Feature | Spec section | Status entry | Compiler evidence | Discrepancy | Resolution |
|---|---|---|---|---|---|
| `prefix`, `suffix`, `infix`, `nop` decls | `Zith--.md` "Fora do Escopo Atual de Zith--" | Parse skipped | `zithc check` on `prefix foo { }` reports `E2010 word declarations are not supported in this version` | none | keep |
| `context` declarations | `Zith--.md` "Fora do Escopo Atual de Zith--" | Parse skipped | `zithc check` on `context foo { body }` reports `E2010 context declarations are not supported in this version` | none | keep |
| `use` statements | `Zith--.md` "Fora do Escopo Atual de Zith--" | Parse skipped | `zithc check` on `use std/io` reports `E2010 use statements are not supported in this version` | none | keep |
| `macro` / `raw macro` and `@name(...)` calls | `Zith--.md` "Macros" | Working | `tests/test-macro.cpp`, `tests/test-frontend-modern-pipeline.cpp::test_pipeline_raw_macro_lowers` | none | keep |
| `tag` calls | `Zith--.md` "Macros" | Parse reported / full Zith | `tests/test-frontend-modern-pipeline.cpp::test_pipeline_tag_macro_is_rejected` | none | keep (limitation preserved) |
| word call expressions | `Zith--.md` "Fora do Escopo Atual de Zith--" | Parse error | `zithc check` on `word foo;` reports `E2001 unknown identifier 'word'` | none | keep |
| word sequence expressions | `Zith--.md` "Fora do Escopo Atual de Zith--" | Parse error | `zithc check` on `1 nop 2;` reports `E2010 word operator sequences are not supported in this version` | none | keep |

## Language Features: Module System And Visibility

| Feature | Spec section | Status entry | Compiler evidence | Discrepancy | Resolution |
|---|---|---|---|---|---|
| `import`, `from`, `export` | `Zith--.md` "Imports Platform-Specific" | Working | `tests/test-frontend-context.cpp::test_default_import_exposes_full_path_namespace`, `::test_export_shared_prefix_fans_out_to_every_submodule`, `tests/test-frontend.cpp::test_structured_imports` | none | keep |
| `c/...` supported runtime surface | `Zith--.md` "Superfície C e Biblioteca" | Working / Partial | `tests/test-stdlib-io.cpp::test_c_fs_portable_surface_checks`, `::test_c_posix_variant_resolves_on_linux`, `::test_c_posix_missing_variant_reports_import_error` | none | keep |
| `alias` | `Zith--.md` "`type` wrapper e `alias`" | Working | `tests/test-nominal-type-debt.cpp::test_alias_remains_transparent` | none | keep |
| `pub`, `mod` | `Zith--.md` "Visibilidade de campos" | Working | `tests/test-frontend.cpp::test_multiple_top_level_decls_with_visibility` | none | keep |
| `mod(..)`, `mod(N)` | `Zith--.md` "Visibilidade de campos" | Working | `tests/test-frontend.cpp::test_struct_field_visibility_parses` | none | keep |
| C header imports | `Zith--.md` "Superfície C e Biblioteca" | Working (validated C) | `tests/test-cinterop.cpp` (14 cases), `tests/test-frontend-modern-pipeline.cpp::test_c_header_import_lowers_external_function` | none | keep |

## Zith Design Surfaces Outside The Zith-- Contract

These rows list Zith design surfaces that are explicitly outside the Zith--
contract. They are not discrepancies: the status document names them as Zith
surfaces, and no beta material claims them for Zith--.

| Feature | Spec chapter | Status entry | Discrepancy | Resolution |
|---|---|---|---|---|
| NRA ownership proof | `nra-spec.md` | Zith only | none | keep |
| `comptime` evaluation | `11-comptime.md` | Zith only | none | keep |
| `try` / `fail` / `with` / `catch` / `throw` | `08-error-handling.md` | Zith only | none | keep |
| Assets (`ZithProject.toml` asset paths) | `12-assets.md` | Zith only | none | keep |
| `.zirl` binary format | `Zith-spec.md` §1.5 | Zith only | none | keep |
| `@appendField`, `@removeField`, `@appendMethod` | `11-comptime.md` | Zith only | none | keep |

## Legacy Reserved Syntax

| Surface | Status entry | Compiler evidence | Discrepancy | Resolution |
|---|---|---|---|---|
| `async fn` | Parse skipped | `zithc check` on `async fn foo() {}` reports `E2010 unexpected token` | none | keep |
| `yield` | Reserved token | `zithc check` on `yield 1;` reports `E2001 unknown identifier 'yield'` | none | keep |
| `fork`, `merge`, `revoke`, `spawn` | Spec only (Zith) | `zithc check` on `fork foo();` reports `E2001 unknown identifier 'fork'` | none | keep |
| `await` | Reserved token | `zithc check` on `await x;` reports `E2001 unknown identifier 'await'` | none | keep |

## CLI Commands

Evidence for the CLI rows is `tests/test-cli-commands.cpp` (33 cases) plus the
probe noted per row.

| Command | Status entry | Compiler evidence | Discrepancy | Resolution |
|---|---|---|---|---|
| `zithc build` | Working | `tests/test-cli-commands.cpp::test_build_derives_codegen_stage`, `::test_check_build_require_explicit_lockfile` | none | keep |
| `zithc run` | Working | `tests/test-cli-commands.cpp::test_run_virtual_machine_executes_program` | none | keep |
| `zithc check` | Working | `tests/test-cli-commands.cpp::test_check_reports_raw_call_marker_requirement` | none | keep |
| `zithc fmt` | Working | `tests/test-formatter.cpp`, `tests/test-cli-commands.cpp::test_command_signatures_exist` | none | keep |
| `zithc create <name>` | Working | `tests/test-cli-commands.cpp::test_create_writes_lockfile_for_generated_project` plus a probe where `zithc create zprobe2` writes `src/main.zithm` and `Zith.lock` | none | keep |
| `zithc clean` | Working | `tests/test-cli-commands.cpp::test_clean_preserves_artifacts_without_confirmation` | none | keep |
| `zithc execute <file>` | Working | `tests/test-cli-commands.cpp::test_command_names_match_contract` | none | keep |
| `zithc test <path>` | Working | `tests/test-cli-commands.cpp::test_test_command_filters_and_executes_tests`, `tests/test-zith-test-suite.cpp` | none | keep |
| `zithc repl` | Stub | probe `zithc repl` prints `[soon] repl not implemented yet` | none | keep (limitation preserved) |
| `zithc deps list` | Working | probe `zithc deps list` in a fresh project prints `[ok] no dependencies` | none | keep |
| `zithc deps add` | Working | `tests/test-cli-commands.cpp::test_deps_add_remove_edit_only_the_manifest` | none | keep |
| `zithc deps remove` | Working | `tests/test-cli-commands.cpp::test_deps_add_remove_edit_only_the_manifest` | none | keep |
| `zithc deps resolve` | Working | `tests/test-cli-commands.cpp::test_deps_resolve_writes_deterministic_lockfile` | none | keep |
| `zithc docs` | Working | `tests/test-docs.cpp` (13 cases) | none | keep |
| `zithc completion <shell>` | Working | `tests/test-cli-commands.cpp::test_completion_command_emits_updated_shell_options` | none | keep |

## Diagnostic Codes

The diagnostic-code table groups ranges, not individual features. It is
verified by `tests/test-diagnostics-render.cpp`, which asserts that each
existing code is preserved and that the new type codes are registered.
`E1013` was added by this audit (`Zith` commit `1942febf`) for the malformed
`import ... from path` form and is registered in the import/parse range.

| Range | Stage | Compiler evidence | Discrepancy | Resolution |
|---|---|---|---|---|
| 0001-0005 | Lexical | `tests/test-diagnostics-render.cpp::test_existing_codes_preserved` | none | keep |
| 1001-1013 | Import/parse | `tests/test-diagnostics-render.cpp::test_existing_codes_preserved` plus the `E1013` probes in "Malformed-Source Fixtures" below | `E1013` was undocumented before the audit | fix (added to the table) |
| 2001-2010 | Semantic | `tests/test-diagnostics-render.cpp::test_existing_codes_preserved` | none | keep |
| 2021-2029 | Frontend/interface | `tests/test-diagnostics-render.cpp::test_existing_codes_preserved` | none | keep |
| 2030-2034 | Semantic | `tests/test-diagnostics-render.cpp::test_existing_codes_preserved` | none | keep |
| 3001-3009 | Types | `tests/test-diagnostics-render.cpp::test_new_type_codes_registered` | none | keep |
| 4001-4008 | SRA / ownership | `tests/test-diagnostics-render.cpp::test_existing_codes_preserved` | none | keep |
| 5001-5002 | Lowering | `tests/test-diagnostics-render.cpp::test_existing_codes_preserved` | none | keep |
| 10001-10004 | Runtime | `tests/test-diagnostics-render.cpp::test_existing_codes_preserved`, `tests/test-codegen.cpp::test_imported_panic_runtime` | none | keep |

## Known Debt And Non-Debt

| Item | Status entry | Compiler evidence | Discrepancy | Resolution |
|---|---|---|---|---|
| Non-literal narrowing conversions need runtime checks | Known debt | `tests/test-hir-lower-modern.cpp::test_numeric_cast_lowers_to_hir_cast` (compile-time only) | none | keep (recorded limitation) |
| `?*T` pointer narrowing is local-only | Known debt | `tests/test-optional-slice.cpp::test_nullable_pointer_narrowing` | none | keep (recorded limitation) |
| `null` in a pack literal | Resolved | `tests/test-hir-lower-modern.cpp::test_anonymous_pack_literal_lowers_to_hir` | none | keep |
| `void`/`never` member in a generic union | Resolved | `tests/test-enum-union-generics.cpp`, `tests/test-codegen.cpp::test_imported_result_runtime` | none | keep |
| Opaque cache divergence diagnostics | Known debt | `tests/test-cache.cpp::test_canonical_registry_reports_actionable_divergence` | none | keep (recorded limitation) |
| Non-debt surfaces (`is <type>` scope, C by-value layouts, C++ interop, user-defined casts, bodyless range `for`, `..`/`...`, `++`/`--`, full NRA) | Non-debt | `tests/test-codegen.cpp::test_bodyless_range_for_is_rejected`, `tests/test-cinterop.cpp` | none | keep |

## Malformed-Source Fixtures

Step 2 Actions 5 to 7 require focused regression cases for malformed imports
and unbraced control-flow bodies, each with an actionable message, a source
span, and a correction grounded in the language contract. The probes below
were run with `zithc check` on 2026-11-24.

| Fixture | Contract | Observed diagnostic | Span | Discrepancy | Resolution |
|---|---|---|---|---|---|
| `import from path` | `Zith--.md` "Imports Platform-Specific" | `E1013` "unexpected 'from' after 'import'; use 'from path' to import symbols, or 'import path' to import a module namespace" | line 1, column 1 | none, one diagnostic, no cascade | keep (regression `tests/test-frontend.cpp::test_import_from_prefix_has_actionable_diagnostic`) |
| `import / from path { foo }` | same | the same single `E1013` message | line 1, column 1 | before the audit this produced a generic `E0001` plus a cascading `E1006` | fix (`Zith` `1942febf`) |
| `if (true) 1;` | `Zith--.md` "Controlo de Fluxo" | `E1012` "expected '{' before if body; write 'if (condition) { ... }'" | line 1, column 23 | none, one diagnostic, correction shown | keep (regression `tests/test-frontend.cpp::test_control_bodies_require_blocks`) |

The `import / from path` fix marks the recovered declaration as an error node,
so resolution does not add the misleading `E1006`. A genuine path segment named
`from` (for example `import graphics/from extra`) is not misread as the
malformed form, and that case is covered by a regression test.

## Public-Claim Findings

These rows are the previously recorded public-facing findings plus the website
source-file references rechecked for this beta. Each was revalidated against
the current compiler and website, not copied from the earlier review.

| # | Claim source | Status entry / contract | Compiler evidence | Discrepancy | Resolution |
|---|---|---|---|---|---|
| 1 | `Zith-website` `zith--/reference.md` "Removed restrictions": `global` listed as functional | `impl-status.md` "let/var/const"; `Zith--.md` "Restrições Removidas" | `zithc check` on `global g: i32 = 5;` reports `E2010 'global' is not supported` | site implied `global` was a working binding | fixed (`Zith-website` `92161c1`, `Zith` `04a6ccbb`) |
| 2 | `Zith-website` `zith--/guide/memory-model.md`: pipeline shown as `NTA/SRA` | `impl-status.md` SRA row; `Zith--.md` "Ownership" | `tests/test-memory-qualifiers.cpp`; `src/session` uses `sraStage()`/`Stage::SraResolved` | wrong analysis name (NTA) | fixed (`Zith-website` `92161c1`, `Zith` `04a6ccbb`) |
| 3 | `Zith-website` `zith--/reference.md`: ownership-arg error shown as `E2007`/`E4007` | `impl-status.md` diagnostics: `E2007` NoMatchingFn, `E4007` InvalidCallOwnership | probe `take(unique x)` on a `lend` parameter reports `E4007` | `E2007` was the wrong code | fixed (`Zith-website` `92161c1`) |
| 4 | `Zith-website` `zith--/guide/syntax.md`: began "Zith source" | Zith-- guide | page belongs to the Zith-- guide | wrong language name | fixed (`Zith-website` `92161c1`) |
| 5 | `Zith-website` `zith--/reference.md` "Platform-specific imports": variants shown as `foo.<arch>.<os>.zith` etc. | `Zith--.md` "Imports Platform-Specific"; `src/support/source-extension.hpp` | `zithc create` writes `src/main.zithm`; module discovery resolves only `.zithm` | wrong extension (`.zith` reserved for flagship Zith) | fixed (`Zith-website` `3097311`) |
| 6 | `Zith-website` `zith--/guide/first-program.md` and CLI pages: scaffold and examples named `src/main.zith` / `repro.zith` | `src/cli/cmd/project.cpp`, `src/session/frontend-module-analysis.cpp` | `zithc create` writes `src/main.zithm`; module candidates resolve only `.zithm` | wrong extension in 9 references across 8 pages | fixed (`Zith-website` `3097311`) |

## Summary

- Every `docs/impl-status.md` status row was rechecked. No row contradicts the
  compiler at revision `29444d4e`.
- The one real compiler gap found in this audit was the malformed
  `import / from path{...}` form, which previously produced a generic `E0001`
  plus a cascading `E1006`. `Zith` commit `1942febf` makes it emit the
  dedicated `E1013` with an actionable message and no cascade. Regressions live
  in `tests/test-frontend.cpp` and `tests/test-wasm-playground.cpp`.
- Six public-claim discrepancies were found across the website review and the
  source-file recheck. All six are fixed and committed.
- No critical or high-priority public-claim discrepancy remains open.
