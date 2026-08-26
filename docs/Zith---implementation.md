# Zith-- Implementation Notes

## Fonte de Verdade

Este arquivo documenta como a toolchain implementa e verifica o subconjunto Zith-- definido em `docs/Zith--.md`. A linguagem compilada pelo `main` é sempre Zith--; não há frontend separado, flag de ativação ou modo opt-in.

## Frontend

O AST modela bindings com `BindingKind`:

- `BindingKind::Let` para `let`.
- `BindingKind::Var` para `var`.
- `BindingKind::Const` para `const`.

`Declaration.bindingKind` é definido para `DeclKind::Variable`. `Binding.bindingKind` é definido para `StmtKind::Binding`. O parser:

- Aplica `Parameter.bindingKind` com default `Let` e lê `var p: T` ou `let p: T` antes do nome do parâmetro; `var self`/`let self` usam o mesmo campo no receiver.
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

Além do check de ownership existente (`checkAssignableOwnership`), `inferAssign` chama
`checkImmutableRootFieldWrite`. Essa função usa `assignmentRoot` para encontrar a raiz da
place expression e rejeita escritas de campo, arrow ou index cuja raiz seja `let` ou `const`
(local ou global) com `Zith--: cannot write through immutable binding '<name>'` (`E2010`).
O check dedicado `checkConstFieldAssignments`/`targetFieldIsConst` continua responsável por
campos `const`; `view` e `lend` mantêm o tratamento de ownership anterior.

`checkImmutableRootFieldWrite` trata também parâmetros de função. Parâmetros comuns têm
`bindingKind == Let` e são read-only; `var p` tem `BindingKind::Var` e permite escrita através
do parâmetro. Receivers explícitos com tipo pointer ou qualificador `lend`/`view` saem do default
read-only (o check de `view` continua em `checkAssignableOwnership`). Bare `self` é read-only,
mas `var self` permite escrita in-place nos campos.

O acesso `self.field` auto-derefs um receiver implícito `*Owner`: `inferField` resolve o pointee
quando o tipo do objeto é pointer, e `HirLowerModern::lowerField` emite um `HirUnaryOp::Deref`
antes de ler/escrever o campo. `self->field` continua no caminho legacy e produz o mesmo acesso.

`PerModuleSema` mantém `movedLocals_`, um dead-state lógico por corpo de função. Ao chamar um
método com `self` implícito ou `var self`, `inferMethodCall` marca a raiz do receiver como movida;
leituras posteriores do nome reportam `E4001 UseAfterMove` e escritas através de campos/índices
do local movido também. Atribuir diretamente ao nome do local (o próprio root) revive a ligação.
Esta fase não altera ABI nem storage e não implementa exclusividade de `lend`/`view` no chamador.

Discriminantes de enum são avaliados em `lowerDeclarationTypes`, não como literais fixos.
O evaluator percorre recursivamente literais inteiros, unários `-`/`~`, binários aritméticos,
bitwise `&.`/`|.`/`^.`, shifts e comparações, variantes anteriores do enum e referências a
`const` inteiros globais/locais. Usa aritmética alargada para detectar overflow de
`int64_t`, depois verifica o resultado contra o tipo subjacente do enum. Expressões não
constantes ou não inteiras reportam `enum variant discriminant must be a constant integer
expression`; valores fora do tipo subjacente são rejeitados com
`enum variant discriminant does not fit its underlying type '<T>'`.

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

A versão de formato ZIRL passa para 10. O Code section serializa:

- `Artifact.exprs` como pool de expressões ao nível do módulo.
- `Artifact.globals` como `CompactGlobalConst` com name, type e init.
- `HirFunction` com `isState`/`machineId`/`machineReturnType`/`usesTailCC` para declaracoes `state`.
- `HirStateTailCall` como expressao de terminacao com `musttail tailcc` direto.

Isso mantém os ids de HIR estáveis entre módulos vazios de funções, const globals, loads por `HirGlobalConstLoad` e transitions `state`. Maquinas `state` agrupam por retorno canonico e permitem listas de parametros diferentes entre estados; codegen declara e chama essas funcoes com LLVM `tailcc` e sem contexto/`alloca` adicional.

For-in usa o protocolo `next(self)` com retorno tagged union de dois membros: um elemento e o `End` canonico (`struct End {}`). O sema exige exatamente um membro `End`; HIR chama `next` no header do loop, ramifica por `HirUnionCheck` quando o tag é `End` e extrai o elemento com `HirUnionCast` quando não é.

## Testes

Os seguintes testes cobrem a iteração:

- `test-frontend`: bindings `let`/`var`/`const`, rejeição de `global`/`mut`/ownership/tags, const fields.
- `test-sema`: valida const global/local, campos const, propagação de imutabilidade em structs/unions, discriminantes constantes de enum e atribuições proibidas.
- `test-hir-lower-modern`: `HirGlobalConst`, `HirGlobalConstLoad` e valores de enum calculados a partir de expressões.
- `test-codegen`: execução runtime de um const global e de uma maquina de estados com `musttail tailcc`, incluindo parâmetros divergentes.
- `test-cache`/`test-zirl-sections`: pool de expressões, globals e state machine metadata persistidos.
- `test-memory-qualifiers`: lend/view aceites; unique/share/belong e mut rejeitados.
- `test-sema`/`test-codegen`: `var p`/`var self`, `self.field` com auto-deref e dead-state `E4001` de receivers pós-método.

Para regressões, usar a suíte completa:

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --build build --target fmt-check
```

Se LLVM não estiver disponível, validar parser/sema/HIR e reportar explicitamente que codegen/cache com LLVM não foi executado.
