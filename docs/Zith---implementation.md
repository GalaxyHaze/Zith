# Zith-- Implementation Notes

## Fonte de Verdade

Este arquivo documenta como a toolchain implementa e verifica o subconjunto Zith-- definido em `docs/Zith--.md`. A linguagem compilada pelo `main` é sempre Zith--; não há frontend separado, flag de ativação ou modo opt-in.

## Frontend

O AST modela bindings com `BindingKind`:

- `BindingKind::Let` para `let`.
- `BindingKind::Var` para `var`.
- `BindingKind::Const` para `const`.

`Declaration.bindingKind` é definido para `DeclKind::Variable`. `Binding.bindingKind` é definido para `StmtKind::Binding`. O parser:

- Aplica o default `BindingKind::Let` e corrige com a palavra-chave real.
- Rejeita `global` antes de baixar a declaração, com recovery para `DeclKind::Variable`.
- Rejeita `const fn` em `functionKindPrefix`.
- Rejeita tag macros em `lowerMacroDeclaration`.
- Rejeita `const name: T` (global ou local) sem inicializador.
- Rejeita `const name: T` sem valor em struct fields.
- Parseia `const X: T = value` em `parseStructField` e marca `Parameter.isConstField`.

Qualificadores `mut`, `unique`, `share` e `belong` são parseados para recovery/formatter, mas emitem `E2010 UnsupportedSyntax`.

## Sema

`checkZithDeclarations` roda dentro de `checkExpressions` e verifica:

- Todo `const` tem inicializador.
- Todo inicializador de `const` é uma expressão constante.
- `let`/`var` com tipo não-trivial têm inicializador.
- Bindings locais `const` têm inicializador.

`isConstantExpression` aceita literais, agregados de literais, struct literals constantes e referências a `const` globais/locais. Outras expressões, incluindo chamadas, são rejeitadas.

Atribuições são verificadas em `inferAssign`:

- `let` com inicializador é imutável.
- `let` sem inicializador aceita a primeira escrita, que pode inferir ou manter o tipo anotado.
- `const` global e `const` local nunca podem ser atribuídos.
- `checkConstFieldAssignments` rejeita escrita em campo marcado `isConstField`.

`targetFieldIsConst` percorre o caminho de uma place expression para detectar campos const, incluindo bases aninhadas e acesso por pointer.

## HIR

`predeclareGlobalConsts` roda antes das funções e cria um `HirGlobalConst` por declaração `const` top-level:

- Nome de linkage `_zith_<module>.<name>`.
- Tipo HIR inferido do decl.
- Inicializador baixado como expressão HIR.

Referências por nome a um const global produzem `HirGlobalConstLoad` no lowering. Consts locais permanecem no caminho local de slots/allocas, imutáveis e com inicializador constante obrigatório.

## Codegen

`emitConstGlobals` é executado antes das funções:

1. Pré-declara todos os `llvm::GlobalVariable` com linkage `InternalLinkage` e `isConstant = true`.
2. Emite cada inicializador.
3. Se o inicializador não for um `llvm::Constant`, reporta erro de codegen.

`HirGlobalConstLoad` faz load do global pelo nome. Não há armazenamento para globals const no emissor.

## Cache e ZIRL

A versão de formato ZIRL passa para 8. O Code section serializa:

- `Artifact.exprs` como pool de expressões ao nível do módulo.
- `Artifact.globals` como `CompactGlobalConst` com name, type e init.

Isso mantém os ids de HIR estáveis entre módulos vazios de funções, const globals e loads por `HirGlobalConstLoad`.

## Testes

Os seguintes testes cobrem a iteração:

- `test-frontend`: bindings `let`/`var`/`const`, rejeição de `global`/`mut`/ownership/tags, const fields.
- `test-sema`: valida const global/local, campos const, atribuições proibidas, expressões constantes e não-triviais sem inicializador.
- `test-hir-lower-modern`: `HirGlobalConst` e `HirGlobalConstLoad`.
- `test-codegen`: execução runtime de um const global.
- `test-cache`/`test-zirl-sections`: pool de expressões e globals persistidos.
- `test-memory-qualifiers`: lend/view aceites; unique/share/belong e mut rejeitados.

Para regressões, usar a suíte completa:

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --build build --target fmt-check
```

Se LLVM não estiver disponível, validar parser/sema/HIR e reportar explicitamente que codegen/cache com LLVM não foi executado.
