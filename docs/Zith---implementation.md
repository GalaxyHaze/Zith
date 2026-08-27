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
- Parseia `pub name: T`, `mod name: T`, `mod(N) name: T` e `mod(..) name: T` antes do nome do
  field e regista `Parameter.visibility`/`Parameter.modDepth`; cada field sem prefixo fica
  `Visibility::Private`. A forma aplica-se também a fields agrupados `[x, y]: T`.

Qualificadores `mut`, `unique`, `share` e `belong` são parseados para recovery/formatter, mas emitem `E2010 UnsupportedSyntax`.

O parser cria `ExprKind::OwnershipCoerce` apenas em listas de argumentos de call (`parseCallArgument()`), usado nas chamadas `f(...)`, `f<G>(...)`, dock e jump. Só `lend` e `view` são aceites como anotação de argumento; `unique`/`share`/`belong`/`mut` nessa posição reportam `E4007 InvalidCallOwnership`. Fora de um call argument, `lend`/`view` continuam a ser tratados como keywords normais e falham com o diagnóstico normal.

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

`PerModuleSema::borrowParamType` converte `lend T`/`view T` de parâmetros livres e self explícitos para `*lend T`/`*view T`; `isBorrowParamType` reconhece esses ponteiros mesmo através de aliases. `checkOwnershipCoercion` roda antes da coerção de tipo em calls livres, chamadas genéricas, overloads e métodos:

- binding `default`/`unique` para parâmetro borrow exige `lend`/`view` no call site (`E4005`);
- argumentos já `lend`/`view`, literais e temporários não exigem anotação;
- anotação incompatível com o parâmetro reporta `E4005`;
- cada call mantém um conjunto de roots anotados; o mesmo root não pode repetir `lend` nem misturar `lend` com `view` na mesma chamada (`E4005`);
- `OwnershipCoerce` mantém o tipo do inner para inferência/overloads; o narrowing real para `*lend T`/`*view T` só acontece depois do check.

O probe de overload usa `coerceValue` para parâmetros borrow, para que `f(lend q)` seja compatível com `fn f(p: lend P)` mesmo quando a ABI semântica é ponteiro.

O acesso `self.field` auto-derefs um receiver implícito `*Owner`: `inferField` resolve o pointee
quando o tipo do objeto é pointer, e `HirLowerModern::lowerField` emite um `HirUnaryOp::Deref`
antes de ler/escrever o campo. `self->field` continua no caminho legacy e produz o mesmo acesso.

`PerModuleSema::fieldVisible` aplica a visibilidade por field usando a `FieldMeta` paralela de
`StructType` (owner, visibility e modDepth). `pub` é sempre visível; `Private` só dentro do
ficheiro do struct; `mod`/`mod(N)`/`mod(..)` segue a mesma regra de profundidade de módulo usada
para declarations. Esta regra é consultada por `inferField`, `inferArrow` e por ambos os caminhos
de struct literal (genérico e concreto). Num literal, um field invisível é rejeitado com o mesmo
diagnóstico de accessor privado e não é contado como field necessário; fields privados não
participam na inferência de argumentos genéricos a partir de literais.

`PerModuleSema` mantém `movedLocals_`, um dead-state lógico por corpo de função. Ao chamar um
método com `self` implícito ou `var self`, `inferMethodCall` marca a raiz do receiver como movida;
leituras posteriores do nome reportam `E4001 UseAfterMove` e escritas através de campos/índices
do local movido também. Atribuir diretamente ao nome do local (o próprio root) revive a ligação.
A exclusividade de `lend`/`view` está implementada no call site; o dead-state lógico de receivers
e o comportamento pós-chamada de funções livres continuam como antes.

`inferMethodCall` reconhece `p.Trait.method()` (AST `Call(Field(Field(p, Trait), method))`)
antes da lookup normal. Quando o receiver é um struct que satisfaz a trait/interface nomeada,
o nó intermediário `p.Trait` é marcado com o tipo concreto e o recebedor real é guardado em
`TypedMap::traitQualifiedReceiverBase`; HIR baixa esse marker como o próprio base, sem emitir um
field que não existe. A seleção de candidatos é então filtrada pelo trait: defaults do trait,
requirements e métodos impl são considerados apenas nesse trait, e interfaces estruturais também
aceitam o método concreto do owner que satisfaz a interface. Sem qualificação, `p.method()`
mantém a seleção atual e continua reportando `E2008` quando dois traits expõem o mesmo nome.

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

`HirLowerModern::lowerExpr` descarta `OwnershipCoerce` e baixa o inner. Em calls, um argumento anotado é baixado como endereço do place: `lowerLValueAddr` para bindings/campos/índices, ou uma alloca temporária quando o operand não tem lvalue. O parâmetro `*lend T`/`*view T` já é baixado como ponteiro. Os slots de parâmetro carregam `HirOwnership::Lend`/`HirOwnership::View` via `NraFacts`, e `NraFacts::localOfArgument`/`ownershipOfArgument` descascam a anotação para continuar acumulando factas por call argument (`NraArgEscape::Borrow` por default).

`checkAssignableOwnership` bloqueia escrita através de parâmetros `*view T` tanto no caminho arrow como no dot-member auto-deref, reportando `E4004`.

### Optional em contexto booleano

`booleanCondition` aceita `ExprKind::OptionalProp` sobre `?T` como `bool`: a condição é
verdadeira quando o optional tem payload e falsa quando é null. Isto aplica-se a condições de
`if`, `while` e `for (cond)`. Fora de condição, `x?` continua a ser o operador de propagação
opcional e exige função de retorno `?T`; `let b: bool = x?` continua rejeitado. `x is null` e
`not (x is null)` continuam a ser os únicos caminhos de narrowing de optional; não existe nesta
iteração `?.` nem `?->`.

`HirLowerModern::lowerOptionalBoolean` baixa `x?` de `?*T` para `Ne` contra `HirMakeNone` (niche
de pointer) e `x?` de `?T` para leitura do discriminante em field index 1. Não gera o braço
`return null` do lowering de propagação, pelo que não é preciso tipo de retorno opcional no
contexto condicional.

## Codegen

`emitConstGlobals` é executado antes das funções:

1. Pré-declara todos os `llvm::GlobalVariable` com linkage `InternalLinkage` e `isConstant = true`.
2. Emite cada inicializador.
3. Se o inicializador não for um `llvm::Constant`, reporta erro de codegen.

`HirGlobalConstLoad` faz load do global pelo nome. Não há armazenamento para globals const no emissor.

Parâmetros HIR com residual de slot `Lend`/`View` recebem `nocapture` no LLVM; `View` recebe também `readonly`. O valor do parâmetro em si continua a ser carregado da alloca do slot, preservando o caminho sem LLVM/legacy para o corpo.

## Cache e ZIRL

A versão de formato ZIRL passa para 10. O Code section serializa:

- `Artifact.exprs` como pool de expressões ao nível do módulo.
- `Artifact.globals` como `CompactGlobalConst` com name, type e init.
- `HirFunction` com `isState`/`machineId`/`machineReturnType`/`usesTailCC` para declaracoes `state`.
- `HirStateTailCall` como expressao de terminacao com `musttail tailcc` direto.

Isso mantém os ids de HIR estáveis entre módulos vazios de funções, const globals, loads por `HirGlobalConstLoad` e transitions `state`. Maquinas `state` agrupam por retorno canonico e permitem listas de parametros diferentes entre estados; codegen declara e chama essas funcoes com LLVM `tailcc` e sem contexto/`alloca` adicional.

## Defer e Cleanup de Escopo

`defer expr;` e `defer { ... }` registam cleanup no bloco lexical mais próximo.
O frontend produz `StmtKind::Defer`; `defer { ... }` guarda o corpo como
`ExprKind::Block` cleanup-only. O sema infere o corpo normalmente, rejeita
`return`/`break`/`continue`/`jump` dentro do corpo adiado e não contribui com o
valor do bloco. O lowering HIR acumula as expressões adiadas e emite
`HirExprKind::Cleanup` em reverse order antes de qualquer terminator do bloco,
incluindo `ret`, branches de `break`/`continue` e `HirStateTailCall`. Codegen
traduz o cleanup para execução imediata antes desses transfers. `state` sem
return type declarado é tratado como `void` e nunca tem tipo inferido do corpo.

For-in usa o protocolo `next(self)` com retorno tagged union de dois membros: um elemento e o `End` canonico (`struct End {}`). O sema exige exatamente um membro `End`; HIR chama `next` no header do loop, ramifica por `HirUnionCheck` quando o tag é `End` e extrai o elemento com `HirUnionCast` quando não é.

## Testes

Os seguintes testes cobrem a iteração:

- `test-frontend`: bindings `let`/`var`/`const`, rejeição de `global`/`mut`/ownership/tags, const fields.
- `test-sema`: valida const global/local, campos const, propagação de imutabilidade em structs/unions, discriminantes constantes de enum e atribuições proibidas.
- `test-hir-lower-modern`: `HirGlobalConst`, `HirGlobalConstLoad` e valores de enum calculados a partir de expressões.
- `test-codegen`: execução runtime de um const global e de uma maquina de estados com `musttail tailcc`, incluindo parâmetros divergentes.
- `test-cache`/`test-zirl-sections`: pool de expressões, globals e state machine metadata persistidos.
- `test-memory-qualifiers`: lend/view aceites; anotações de call `E4005`/`E4007`, exclusividade por call e views read-only; unique/share/belong e mut rejeitados.
- `test-sema`/`test-codegen`: `var p`/`var self`, `self.field` com auto-deref e dead-state `E4001` de receivers pós-método.
- `test-sema`: overload/generic com parâmetros lend/view e mismatch de tipo que continua a reportar `E2007` sem mascarar `E4005`.
- `test-hir-lower-modern`: parâmetro livre `lend` baixa para ponteiro e call passa endereço do binding.
- `test-codegen`: parâmetro livre `lend` muta o binding do chamador; `view` lê sem escrever.

Para regressões, usar a suíte completa:

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --build build --target fmt-check
```

Se LLVM não estiver disponível, validar parser/sema/HIR e reportar explicitamente que codegen/cache com LLVM não foi executado.
