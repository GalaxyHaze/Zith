# Zith Implementation Debt

> Last updated: 2026-08-31.

Documento de gestão da dívida de implementação. Distingue propositadamente:

- **Dívidas reais**: funcionalidade que foi implementada de forma incompleta, com
  limitação conhecida que não é a intenção de design, ou código que repete lógica
  e dificulta manutenção.
- **Não-dívidas**: decisões de design intencionais para `Zith--`; manter o
  comportamento atual, mesmo que pareça incompleto comparado com o spec maior do
  Zith.

Este ficheiro não substitui `docs/impl-status.md`; é o inventário de trabalho de
engenharia para rever e gerir.

---

## Não-dívidas (decisões de design)

| Item | Decisão |
|---|---|
| `const fn` | Não é pretendido em `Zith--`. O parser aceita `FunctionKind::Const`, mas o pipeline rejeita `const fn` com `UnsupportedSyntax` em [frontend-decl.cpp](/home/diogo/Zith/src/frontend/frontend-decl.cpp:406). Não documentar como dívida. |
| `dyn Interface` sem acesso a fields | O design expõe apenas métodos em `dyn`; fields ficam disponíveis em tipos concretos e bounds genéricos. `a.x on dyn Interface` com `E3001` é comportamento pretendido, não debt. |

---

## Dívidas de features implementadas mas incompletas

### 1. `type Name = T` é nominal mas sem sintaxe completa

- Estado atual: `type Name = T` cria um wrapper nominal de um campo; não é
  intercambiável com `T`.
- Incompleto: não existe sintaxe explícita de construção e acesso. Existem casts
  de wrapper/unwrapper (`T as Name` / `Name as T`), mas a superfície é posta como
  `Partial` em [impl-status.md](/home/diogo/Zith/docs/impl-status.md:79).
- Decisão em aberto: definir sintaxe de construção/acesso ou declarar a forma
  atual suficiente para `Zith--`.

### 2. Cache ainda não usa `.zirl`

- Estado atual: o object cache funciona e realiza hits; o formato `.zirl` não é
  produzido nem consumido.
- Risco: estado completo do artefacto não é persistido numa representação estável;
  invalidações e round-trips dependem do array de object files.
- Referência: [impl-status.md](/home/diogo/Zith/docs/impl-status.md:41).

### 3. NRA está parcial

- Estado atual: facts residuais e call annotations existem e são consumidos antes
  do lowering final.
- Faltas reais: o state machine completo alive/dead/lent e a prova de quatro
  regras não existem; não há todos os diagnósticos de ownership previstos.
- Referência: [impl-status.md](/home/diogo/Zith/docs/impl-status.md:38).

### 4. Bare `opaque` não pode ser re-hidratado no cache/cross-module

- Estado atual: `opaque` funciona dentro de um módulo; o typeId é determinístico
  mas module-local.
- Dívida: valores `opaque` importados ou vindo de cache são rejeitados com
  `E2010`; falta um registry de typeIds cross-module ou uma forma de
  canonicalizar os tags no artefacto cacheado.
- Pontos de bloqueio: [compilation-session.cpp](/home/diogo/Zith/src/session/compilation-session.cpp:628) e
  [hir-lower-expr.cpp](/home/diogo/Zith/src/sema/hir-lower-expr.cpp:708).

### 5. C interop é `Working (common C)`, não ABI completa

- Estado atual: libclang cobre C comum, variadics, parâmetros array-decayed,
  `va_list` e function pointers; macros object-like escalares são importadas.
- Dívida real: struct-by-value ABI não é verificado; bitfields, packed/anonymous
  records, flexible arrays, globals e strings não são importados.
- Referência: [impl-status.md](/home/diogo/Zith/docs/impl-status.md:146).

### 6. Outras incompletudes já registadas

- Literal ranges (`1..4`) e range syntax continuam sem sema dedicada.
- `is <type>` fora de unions/opaque não existe.
- Narrowing após `is null` não existe; `?*T -> *T` unchecked permanece.
- Casts numéricos estreitantes não verificam overflow.
- `++` / `--` não existem.
- Formatter reimprime `for (cond)` como `while`.
- `..` é lexado caractere a caractere.

Estas entradas são detalhadas em [impl-status.md](/home/diogo/Zith/docs/impl-status.md) na secção
`Known Debt` e devem ser movidas/consolidadas aqui quando forem tratadas.

### 7. Falhas remanescentes conhecidas em `test-codegen`

Estado de 2026-09-01: `./build/test-codegen` reporta `364 passed, 6 failed`.
Estas falhas não pertencem ao contrato stdlib I/O e ficam para tratamento
separado:

- Pointer indexing inválido por ownership: `E4008 pointer to local storage
  cannot escape the current scope` em dois verificações; a validade/borderline
  destes programas deve ser revista.
- `when` com ramos literal, range e default falha na compilação/dispatch;
  precisa de revisão no frontend, sema ou lowering de ranges/`when`.
- Qualified `lend`/`view` receivers falham verificação de IR (`E5001`);
  o receiver mutante em `*T` qualificado ainda não é gerado de forma válida.

Não avançar com estas correcções enquanto dura o trabalho de stdlib I/O, para
manter o diff focado.

### 8. `ParseInput` / `InputLine.cast<T>` ficou adiado

- Estado: o contrato stdlib documenta `ParseInput` e `input().cast<T>` para
  `bool`, `f32`, `f64`, `u32` e `*char`, mas o subset actual não suporta traits
  com `Self` como retorno nem chamadas estáticas a um tipo genérico
  (`T.parse(self)`); `line.cast<T>()` também não resolve como método genérico.
- Dívida real: falta suporte a trait assinaturas polimórficas com `Self` no
  retorno (`?Self`) para primitivos, ou uma solução de parsing sem trait
  genérica (por exemplo métodos sobrecarregados por tipo no `InputLine`).
- Decisão durante esta iteração: não bloquear `print`/`println`/`input` por esse
  suporte; o `InputLine` actual devolve buffer e o parsing será adicionado num
  passo posterior.

---

## Dívida de estrutura: monolitos

Os ficheiros abaixo concentram demasiado pipeline por ficheiro. A prioridade é
quebrá-los por responsabilidade sem alterar comportamento.

| Ficheiro | Linhas atuais | Quebra proposta |
|---|---|---|
| `src/session/compilation-session.cpp` | ~1985 | separar pipeline de stages, cache e link/exec |
| `src/session/frontend-context.cpp` | ~1789 | separar cache/module executor, análise de módulos e resolução de símbolos |
| `src/codegen/codegen-emit.cpp` | ~1206 | separar emissão por área (params, expr, control flow) |
| `src/sema/hir-lower-expr.cpp` | ~2140 | candidates secundários ainda acima de 1000 linhas |
| `src/frontend/frontend-expr.cpp` | ~1077 | candidates secundários ainda acima de 1000 linhas |

Estado da quebra de `frontend.cpp`:

- `frontend.cpp`: snapshot/reconstruct/parse/canonical/functionSignature e
  orquestração pública (~199 linhas).
- `ast-lowerer.cpp`: lexer, CST builder, helpers do lowerer, `run()` e
  `skipMacroInvocation()` (~667 linhas).
- `frontend-types.cpp`: `parseType`, `isIntrinsicName` (~325 linhas).
- `frontend-expr.cpp`: call args, primary, postfix, expression/binary expression
  precedence e associativity (~1077 linhas).
- `frontend-stmt.cpp`: blocks, if/when, loops, condition, tag macro e statements
  (~1027 linhas).
- `frontend-decl.cpp`: imports, macros, implement, `lowerDeclaration`, campos de
  struct/interface e skip helpers (~1016 linhas).

O header `frontend/ast-lowerer.hpp` expõe `AstLowerer`, helpers de token e
`lex`/`parseCst`/`lowerAst`. `FrontendSnapshot` concede `friend` a
`lex`, `parseCst`, `lowerAst`, `AstLowerer` e `MacroExpander`.

Estado da quebra de `sema-modern.cpp`:

- `sema-modern.cpp`: construtor, `run()`, `prepareTypes()`, `checkExpressions()`,
  accessors, report helpers e `SemaPipeline` (182 linhas).
- `sema-decl.cpp`: registo de tipos, lowering de declarações, implement blocks,
  defaults de structs/functions.
- `sema-type.cpp`: lowering de tipos, foreign types, instanciação e resolução de
  declarações/interfaces.
- `sema-expr.cpp`: dispatcher de inferência e operadores unary/binary.
- `sema-call.cpp`: overload resolution, calls, variadic tail e default args.
- `sema-method.cpp`: métodos, `dyn`/traits/interfaces e constraints genéricas.
- `sema-control.cpp`: blocks, controlo de fluxo, defer, loops e returns.
- `sema-cast-coerce.cpp`: casts, coercions, narrowing e `opaque`.
- `sema-assign.cpp`: assignments, ownership, moves e raw reads.
- `sema-index.cpp`: index, field/arrow, enums e visibilidade.
- `sema-literal.cpp`: struct/array/pack literals, unions e defaults.
- `sema-state.cpp` / `sema-state-access.cpp`: state machines, dock/jump e
  resolução de nomes/accessors.
- `sema-zith.cpp`: const semantics, Zith-- checks e unificação.
- Helpers partilhados em `sema-modern-utils.{hpp,cpp}`.

Próxima fronteira:

- separar `frontend-context.cpp`: `ContentFingerprint`, `SourceCatalog`,
  `ImportRequest`/`ModuleCache` e resolução de módulos são agrupáveis por
  responsabilidade.
- separar `compilation-session.cpp`: stages do pipeline ficam em
  `session/compilation-session.cpp`; link/exec/cache podem sair para TUs
  dedicadas.
- emitir `codegen-emit.cpp` em áreas menores quando for prioridade.

Para o HIR lowering, a fronteira candidata foi já executada:

- `hir-lower-modern.hpp` continua a classe principal e o estado partilhado.
- `hir-lower-types.cpp`: `lowerType`, `lowerTypeExprConcrete`, `lowerForeignType`,
  `lowerTypeSize`, `lowerTypeAlign`, `lowerTagType`, `taggedMemberIndex`,
  `stableConcreteTypeId`.
- `hir-lower-expr.cpp`: `lowerExpr`, literals, nomes, unary/binary, field/arrow,
  index, slice, literal aggregates, casts, intrinsics, coercions e optional
  payloads.
- `hir-lower-call.cpp`: forms de call não-dyn e dyn, default args, variadic slice
  tail, `dyn` dispatch e tail calls.
- `hir-lower-block.cpp`: `lowerBlock`, `defer`, `if`, `when`, loops e condicoes.
- `hir-lower-stmt.cpp`: `lowerStatement`, bindings, return/break/continue e
  transições `state`/`jump`.
- `hir-lower-util.cpp`: helpers anónimos partilhados (`decodeEscapes`,
  `internFunctionKey`, `moduleNamespace`, `mapHirOwnership`,
  `mapHirEscape`).

O glob de `src/*.cpp` no CMake recolhe os novos ficheiros automaticamente; depois
de criar ficheiros, é preciso reconfigurar (`cmake -S . -B build`) para o glob
ver os novos `.cpp`.

---

## Dívida de duplicação e padrões repetitivos

### Duplicação de variadic tail logic

A lógica de `explicit_slice_arg` vs `auto_collect_tail` aparece repetida entre
`src/sema/sema-modern.cpp` (linhas ~2585, ~3245, ~3403, ~5577, ~5676) e
`src/sema/hir-lower-call.cpp` (linhas ~205, ~420 e ~505). O lowering reimplementa a
decisão de sema com regras ligeiramente locais, o que cria risco de divergência
quando o comportamento de calls variadic muda.

Acção recomendada: centralizar a decisão em sema (por exemplo um `VariadicCallPlan`)
guardado no snapshot ou no nó typed do call, e o lowering consumir apenas esse
plano.

### Concatenação ProjectConfig + Options

Vários pontos de `src/session/compilation-session.cpp` repetem o padrão de
`dynArr.insert(end, mProjectConfig.X...)` + `mOpts.get().X...`:

- `includeDirs`: linhas ~353 e ~996.
- `cSourceDirs`: linha ~958.
- `defines`: linhas ~358 e ~1011.
- `libraryDirs` e `libraries`: linhas ~1142 e ~1151.

Acção recomendada: helper única `mergeStrings(config, options, field, append)`
para evitar erros de ordem e duplicação.

### Erro de `opaque` duplicado

A mensagem de `E2010` para `opaque` module-local está duplicada pelo menos em
[compilation-session.cpp](/home/diogo/Zith/src/session/compilation-session.cpp:628) e em dois
ramos de [hir-lower-expr.cpp](/home/diogo/Zith/src/sema/hir-lower-expr.cpp:708).

Acção recomendada: diagnostic helper único ou uma constante partilhada com span.

### Split inicial por script deixou includes colados e métodos órfãos

Estado resolvido: includes colados foram partidos, `isOpaquePointerCast` foi
restaurado em `sema-cast-coerce.cpp`, e os helpers de interface/resolução de
`sema-modern.cpp` foram movidos para `sema-type.cpp`.

Risco residual: o split foi mecânico; a estrutura dos ficheiros é razoável, mas
a localização de cada método deve ser revista quando se mexer na área para
confirmar que está no ficheiro com a responsabilidade certa.

### HIR nodes sem initializers completos

`cpp_check` reportou vários `uninitMemberVarNoCtor` em `src/hir/hir-expr.hpp` a
partir da linha 86. A maioria é fixada pela ordem de preenchimento em
`hir-lower-modern.cpp` antes de `addExpr`, mas o padrão é frágil: basta mover um
agregado para outra TU ou adicionar um builder que omita um campo para obter valor
indeterminado.

Normas aplicáveis:

- AUTOSAR `A8-5-0`: toda a memória deve ser inicializada antes de ser lida.
- MISRA C++ `8-5-1`: todas as variáveis devem ter valor definido antes de uso.
- C++ Core Guidelines `C.41`: um construtor deve criar um objecto totalmente
  inicializado.

Acção recomendada: dar default member initializers ou construtores dedicados aos
nodes HIR, mantendo a política do projeto de não usar excepções/RTTI.

---

## Próximos passos para rever

1. A quebra de HIR, de `sema-modern.cpp` e de `frontend.cpp` está feita; a
   próxima prioridade é `frontend-context.cpp`, `compilation-session.cpp` ou
   `codegen-emit.cpp`, conforme o risco da área.
2. Em cada extracção, compilar `zithcLib` e correr os testes da área afectada;
   `ctest --test-dir build --output-on-failure` para regressões gerais.
3. Casos de incompletude que precisam de decisão de produto (sintaxe de `type`,
   slices literais, `is <type>`) devem ser tratados como issues separados, não
   como parte da quebra mecânica.
4. Consolidar as entradas duplicadas de `Known Debt` de `docs/impl-status.md`
   para este ficheiro quando forem tratadas.
