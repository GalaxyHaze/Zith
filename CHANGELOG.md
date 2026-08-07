# Changelog

## [Não Lançado] — `examples/` como suite de aceitação executável

### Testes

- **`test-examples`:** novo teste CTest que compila, liga e executa todos os programas de
  `examples/` através do CLI `zithc`, comparando o exit code com o valor documentado no
  cabeçalho de cada exemplo. Registado sob `ZITH_HAS_LLVM` porque exige codegen e linker.
  Cada exemplo é copiado para `build/examples-run/` antes de correr, porque o `zithc` escreve
  `target/` e `cache/` junto ao ficheiro fonte e correr in-place sujaria a árvore de trabalho.
- **8 exemplos novos:** `for-loops` (as três formas de `for` e os compostos aritméticos),
  `bitwise` (`&.` `|.` `^.` `~`, shifts, compostos e literais `0b`/`0x`/`0c`), `when-dispatch`
  (arms literal, range, guarda booleana e `(_)`), `arrays` (literais, `[N]T`, `@sizeOf`),
  `nested-structs` (structs aninhados, `&`, `->`, `*`), `optionals` (`?T`, propagação `?`,
  `is null`, `?*T`), `c-interop` (`printf` variádico e `malloc`/`free`, só com
  `ZITH_ENABLE_C_INTEROP`) e `generics-decl` (listas `<T>` em `struct`/`alias`).
- **`hello-world.zith`:** passa a declarar `fn main(): i32` com `return 0` explícito. Sem tipo
  de retorno o exit code era lixo não inicializado (64 nesta máquina), o que impedia asserção.

## [Não Lançado] — Operadores Compostos, Bitwise e `raw opaque`

### Operadores

- **Atribuição composta:** `+= -= *= /= %= <<= >>= &= |= ^=` são reescritos no parser para
  `Assign(Binary(base))`, pelo que produzem valor tal como `=` e herdam a coerção e a
  verificação de `view` (`E4004`). Não há nó HIR novo nem bump de versão do ZIRL.
- **Bitwise base:** `&.` `|.` `^.` e o unário `~` passam a ser operadores reais, com as
  precedências da especificação (`|.` < `^.` < `&.` < shifts). Ambos os operandos têm de ser
  inteiros do mesmo tipo; reutilizam `HirBinaryOp::And`/`Or`/`Xor` e `HirUnaryOp::BitNot`.
- **Associatividade:** a atribuição é agora associativa à direita, logo `a = b += 1` é
  `a = (b += 1)`.
- **Lexer:** maximal munch longest-first sobre uma tabela explícita (três caracteres antes de
  dois). `..` continua a lexar carácter a carácter para não afectar os padrões de range.
- **Fix de miscompilação silenciosa:** `var c: i32 = a && b;` compilava e devolvia `a`, porque
  `&` tinha precedência -1 e o parser abandonava o ciclo binário descartando `&& b`. `&&` e `||`
  são agora lexados como um token só para reportarem um erro dedicado a apontar para
  `and` / `or`, com exactamente um diagnóstico.

### Tipos

- **`raw opaque`:** novo `TypeExprKind::Opaque`, reduzido a ponteiro-para-void. `as` converte
  entre `raw opaque` e qualquer `*T` nos dois sentidos; ponteiro-para-ponteiro entre pointees
  concretos e misturas inteiro/ponteiro continuam a reportar `E3003`. `*void` escrito
  literalmente continua rejeitado, e `opaque` isolado continua a reportar `unknown type`.
- **Casts e coerção de ponteiros C:** desde que um ponteiro importado de C passou a ser `?*T`,
  `as` e a coerção deixavam de o reconhecer como ponteiro: `malloc(64) as ?*i32` reportava
  `E3003` e passar qualquer ponteiro a um parâmetro `void*` reportava `E2007`. `as` passa a
  desembrulhar um nível de `Optional` em cada lado, e o resultado é exactamente o tipo escrito.
  Descartar a nulabilidade continua proibido: `malloc(64) as *i32` reporta `E3003` com uma
  mensagem dedicada a apontar para `as ?*T`. Em sentido inverso, qualquer `*T` ou `?*T` coage
  implicitamente para `raw opaque` / `?*void` (argumentos e retornos), pelo que `free(x)`
  compila sem cast; a coerção é unidirecional. Numa chamada sobrecarregada, a assinatura exacta
  ganha da que só encaixa por esta abertura, para não introduzir `E2008`.

## [Não Lançado] — C Interop Completo e Fixes de Literais/Variádicos

### C Importer

- **Parâmetros decaídos:** arrays (`char buf[20]`, `int m[3][4]`), `va_list` e ponteiros para
  funções importam através do tipo de função já decaído pelo libclang; parâmetros array que
  ainda cheguem pré-decay são aceites como ponteiro opaco.
- **Diretório resource do clang:** `ZITH_CLANG_RESOURCE_INCLUDE_DIR` é detectado no configure
  CMake a partir do pacote LLVM e adicionado ao probe e ao parse de headers (`stddef.h` deixa de
  falhar em `import "stdio.h"`).
- **Roots de sistema:** `systemIncludeDirs()` é descoberto/memoizado por `(triple, sysroot)`,
  entra na chave de cache e é passado ao libclang depois dos `-I` do projecto.
- **Skips não fatais:** declarações C não representáveis são saltadas em vez de falharem o
  import; o motivo fica em `skippedFunctions`. Glibc-style duplicados (`asm`-labels) colapsam no
  primeiro binding.

### Literais, Escapes e Variádicos

- **Char literals:** `'B'` inferido como `char`; `HirLiteral` e codegen emitem `i8`; `as char`
  aceito como conversão inteira.
- **Escapes:** `\n \r \t \0 \\ \' \" \xHH` são descodificados partilhando um decoder entre
  strings e chars; escapes desconhecidos reportam E0001.
- **Variadic promotions:** a cauda de chamadas C variádicas promove `f32` para `f64` e
  `bool`/`char`/inteiros pequenos para `i32`, usando a assinatura original do argumento para a
  extensão correcta.
- **Crash fix:** literais com tipo `error` devolvem `nullptr` em vez de chamarem
  `getNullValue(void)`.

### CLI

- **`zithc run` já não engole o output do programa:** o stdout/stderr capturado do processo filho
  é escrito no **stdout** do `zithc` depois da execução (bytes verbatim, via `fwrite`), enquanto os
  diagnósticos do compilador continuam no stderr. Antes, `flushOutput()` era chamado *antes* de
  `linkAndExec()` e os bytes capturados morriam com a sessão; só o exit code sobrevivia.
- **Separação de buffers:** `CompilationSession::flushOutput()` devolve apenas texto do compilador;
  o output do programa sai por `takeChildOutput()`.
- **`zithc run`/`execute` são transparentes para o programa:** o processo filho **herda** o
  `stdout`/`stderr` do terminal (`CompilationSession::linkAndExecDirect()`), pelo que output
  intermédio sem newline, prompts e stdin interactivo funcionam. `linkAndExec()` +
  `takeChildOutput()` mantêm-se para testes e clientes embutidos.
- **Dumps `--emit-tokens`/`--emit-ast` na banda do compilador:** passam por `writeOutput()` como o
  HIR/IR/ASM, logo em `run` saem no stderr e nunca contaminam o stdout do programa.
- **`zithc build <ficheiro>` produz binário:** `deriveTargetStage()` leva `Command::Build` até
  codegen e o `build` liga o objecto via `CompilationSession::link()` (sem executar). `--emit
  obj/ir/asm/hir` mantêm o comportamento anterior.

### Testes e Docs

- Aceitação `import "stdio.h"` + `printf("v=%d\n", 42)` corre e imprime exactamente `v=42\n`.
  Cobertura nova em `test-cinterop`, `test-sema` e `test-codegen`; execução do child captura
  stdout/stderr para permitir asserções byte-exactas.
- `docs/18-c-interop.md`, `docs/impl-status.md` e `docs/roadmap.md` actualizados.


## [Não Lançado] — Macros no Pipeline e Tag Macros

### Frontend / Sema / HIR

- **Resolução por id de nó:** `findResolvedExpr` usa `ExprId` em vez de span, eliminando as
  colisões causadas pela expansão de macros (todos os nós expandidos partilhavam o span da
  call-site).
- **Corpo-template inerte:** os corpos de `macro`/`raw macro`/`tag macro` deixam de ser
  analisados como código real; só os clones numa call-site são compilados.
- **Higiene corrigida:** argumentos clonados da call-site já não são renomeados; o `t` do
  chamador permanece referenciável dentro do corpo expandido.
- **Tag macros:** nova keyword `tag` aceite como `tag macro`; invocação
  `<Section attr: valor> ... </Section>`, atributos `name: value`, um argumento `body` com o
  conteúdo, substituição de `attributes.name`, e códigos `E2017`–`E2020` para valor de tag,
  tag desemparelhado, atributo desconhecido e atributos não declarados.

### Documentação

- `docs/15-macros.md` actualizado para a sintaxe actual; `docs/impl-status.md` e
  `docs/roadmap.md` marcam `F-08` e `F-24` como implementados.



## [Não Lançado] — Wave 01 (2026-08-03) — Infraestrutura de Ship-Readiness

### ZIRL, cache, CLI, diagnósticos, roadmap e testes de infra

- **ZIRL Sections:** Refatoração da serialização monolítica em secções tipadas (`encodeTypes`/
  `decodeTypes`, `encodeDecls`/`decodeDecls`, `encodeCode`/`decodeCode`, `encodeDebug`/
  `decodeDebug`). `Writer::write` e `Reader::read` mantêm API pública inalterada e byte-idêntica.
  Secção `Debug` como secção reservada válida (payload vazio, reader ignora).
- **Cache Hydration:** `CacheEntry` com artifact + fingerprint validado. `tryLoadPersistentCache()`
  persiste a entrada hidratada para os short-circuits do pipeline. Invalidação transitiva via
  `Manifest::dependentsOf` com protecção contra ciclos. `StoreMetrics` exposta via `metrics()`.
- **CLI:** `zithc test <path>`, `zithc deps list`, `zithc docs` implementados com exit codes
  correctos. Flag `--cache-stats`.
- **Diagnostics:** Novos códigos dedicados para TypeMismatch (3001), CannotInfer (3002),
  CyclicType (3004), NullDerefUnproven (3005). Códigos existentes (E0001, E2002, E1006, E3003,
  W1008) preservados. `labels` e `suggestions` visíveis no renderer. `SourceMap` como consulta
  primária de linha, `findLine` como fallback.
- **Roadmap & Tests:** `docs/roadmap.md` com IDs estáveis (F-01 a F-32) e dependências.
  `docs/impl-status.md` corrigido contra o código real. Quatro novos executáveis de teste
  registados: `test-zirl-sections`, `test-cache-entry`, `test-cli-commands`,
  `test-diagnostics-render`.
## [Não Lançado] - 2026-07-29 (correções de linguagem)

### Operadores multi-char, `for` vs `while`, `as` explícito, ponteiros não-nuláveis, `is null`

#### Lexer
- Maximal-munch para `==`, `!=`, `<=`, `>=`, `->`: passam a ser um único token. Antes o lexer
  emitia um token por caractere, pelo que **todos** os operadores de comparação de dois
  caracteres eram rejeitados pelo parser.
- Os restantes operadores multi-char (`&&`, `||`, `+=`, `<<`, `>>`, `..`) continuam divididos por
  caractere: a sua `precedence()` é -1 e tokenizá-los penduraria o loop binário.

#### Frontend Parser
- `ExprKind::Cast` (`expr as Tipo`, com `Expression::cast_type`) e `ExprKind::IsNull`
  (`expr is null`). `as` é postfix, logo liga mais forte que qualquer operador binário;
  `is null` tem a precedência das comparações.
- `is` com qualquer outro operando reporta "only 'is null' is supported in this version".
- Novo `parseFor()`: `for { ... }` (condição sintética `true`) e `for (cond) { ... }` produzem
  `ExprKind::While` — mesma estrutura de blocos, sem novo lowering. As formas iteradoras
  (`for (x in xs)`) e de 3 cláusulas reportam "not implemented yet".
- `while` continua funcional mas emite um aviso de deprecação (`Diagnostic::isWarning`).

#### Diagnósticos
- `err::DeprecatedSyntax` (W1008) e encaminhamento de avisos do snapshot mesmo quando o
  `check` passa — antes o aviso era forçado a erro e nunca chegava ao utilizador.

#### Sema Moderno
- `classifyCast(from, to)` — único ponto de decisão da política de conversão; nesta iteração só
  aceita pares numéricos (Integer/Float) e identidade. Casts de ponteiro e definidos pelo
  utilizador entram depois como um ramo novo aqui.
- `inferCast` e `inferIsNull` (`is null` exige operando `?T`; produz `bool`).
- `inferWhile` passa a inferir também o corpo do loop — sem isto os locais declarados dentro de
  um loop não tinham tipo registado.
- Sem conversões numéricas implícitas: só literais se adaptam ao tipo anotado
  (`adaptNumericLiteral`). `reportCoercionFailure` centraliza a mensagem em todos os call sites
  (atribuição, inicializador, argumento, campo de literal, retorno).
- `*void` e `?*void` rejeitados: "use 'raw opaque' for C interop".
- `null` num ponteiro simples rejeitado com "cannot assign 'null' to a non-optional pointer;
  use '?*T'".

#### Tabela de Tipos
- `TypeTable::canonical(TypeId)` — resolve um placeholder nominal para o tipo completo registado
  com o mesmo nome. Corrige structs auto-referenciais via `?*Node`.

#### HIR Lowering Moderno
- `HirCast` (`value`, `from`, `to`) no `HirExprKind`, na variante `HirExpr`, e no dump.
- `lowerIsNull`: `?*T` usa o niche do nullptr (`Eq` contra `MakeNone`); `?T` não-ponteiro lê o
  discriminante (`Field{index 1}`) e nega-o (`Unary{Not}`).
- `lowerType` para structs passa a copiar os campos para o tipo lowered — antes as structs
  chegavam ao LLVM vazias (`%zith.struct.0 = type {}`).
- `lowerStructLiteral` reordena os valores para a ordem de declaração e embrulha `T` em Some
  para campos `?T`.

#### Codegen
- Visitor de `HirCast`: Trunc/SExt/ZExt, FPTrunc/FPExt, SIToFP/UIToFP, FPToSI/FPToUI.
- `emitFieldAddr` trata slots, ponteiros e agregados em registo — corrige o erro E5001
  "Basic Block does not have terminator" em `return p.y;`.
- `emitUnary` trata address-of antes de avaliar o operando, para que `&local` não emita um load.

#### Formatter
- Casos para `Cast` (`expr as Tipo`) e `IsNull` (`expr is null`).

#### Testes e Exemplos
- `examples/linked-list.zith` — programa de aceitação end-to-end: lista ligada com `?*Node`,
  travessia com `for (not (cur is null))`, `==`/`!=`, e conversões `as`. Sai com código 7.
- 21 novos casos: 7 em `test-frontend.cpp`, 8 em `test-sema.cpp`, 4 em
  `test-hir-lower-modern.cpp`, 4 em `test-codegen.cpp` (incluindo as regressões do
  `type {}` e do `return p.y`).
- Suite completa: 23/23 testes passam.


## [Não Lançado] - 2026-07-29 (late)

### Pipeline Moderno — Structs, Ponteiros, e Acessos a Campos

#### Frontend Parser
- `ExprKind::Field`, `ExprKind::Arrow`, `ExprKind::StructLiteral` — três novos tipos de expressão.
- `Expression::field_names` — vetor paralelo de nomes de campos para literais de struct.
- Suporte a parsing de declarações de campos em structs (`{ nome: Tipo, ... }`) no `AstLowerer::lowerDeclaration`.
- Suporte a acesso `.campo` (dot), `->campo` (arrow, dessugar para deref + dot), e literais de struct `Nome { campo: expr, ... }`.
- Operadores unários de prefixo `&` (addr-of) e `*` (deref) adicionados ao `parseExpression`.

#### Tabela de Tipos
- `StructType::field_names` — array paralelo de nomes de campos no tipo struct.
- `TypeTable::internStruct` aceita um parâmetro opcional `field_names`.
- `TypeTable::fieldIndex` — novo helper para lookup de índice de campo por nome.

#### Sema Moderno
- `lowerDeclarationTypes` para structs agora popula tipos e nomes de campos via `internStruct`.
- `inferField`, `inferArrow`, `inferStructLiteral` — inferência de tipos para os três novos `ExprKind`s.
- `inferUnary` estendido para `&` (produz ponteiro) e `*` (desreferencia ponteiro).

#### HIR Lowering Moderno
- `lowerField` — emite `HirField` com índice de campo resolvido por nome.
- `lowerArrow` — emite `HirUnary{Deref}` seguido de `HirField`.
- `lowerStructLiteral` — emite `HirStructLiteral` com os valores dos campos.
- `lowerAssign` estendido para suportar alvos lvalue do tipo `Field`/`Arrow`.
- `mapUnaryOp` estendido com `Ref` e `Deref`.

#### Formatter
- Suporte a `Field`, `Arrow`, e `StructLiteral` no `FmtVisitor` (16 `ExprKind`s cobertos).

#### Testes
- `test-hir-lower-modern.cpp`: 4 novos casos — struct literal, dot field read, addrof/deref, arrow access.
- Suite completa: 23/23 testes passam.

## [Não Lançado] - 2026-07-15

### Modificações Realizadas

#### Análise Semântica e Barreira de Sintaxe Experimental
- **Erro Semântico E2010 (`UnsupportedSyntax`)**: Criado um diagnóstico de erro semântico específico para reportar sintaxes que são analisadas pelo parser mas ainda não são suportadas pelo compilador.
- **Barreira de Semântica antes do HIR**: Implementada rejeição explícita na fase de análise semântica (`SemaPipeline`), impedindo que as seguintes construções cheguem ao Lowering de HIR ou Codegen:
  - Expressões `is` e `as`.
  - Operadores unários novos de fallback opcional/failable (`?` e `!`) e propagação opcional/failable (`?` e `!`).
  - Sequências de operadores customizados (`SeqNode`).
  - Chamadas de operadores customizados (`WordCallNode`).
  - Instruções de importação `use`.
  - Declarações de palavras-chave customizadas (`WordDeclNode`) e blocos de contexto (`ContextDeclNode`).
- **Remoção de Mapeamento Provisório no HIR**: Removido o mapeamento temporário em `src/sema/hir-lower.cpp` que reduzia incorretamente as novas operações para `Add`/`Neg`. Agora, qualquer tentativa de rebaixar uma dessas ASTs para o HIR resulta em abort (`std::abort()`).

#### Parser e Léxico
- **Suporte para Sintaxe `use`**: Ajustado o parser para aceitar corretamente as sintaxes documentadas `use SQL;`, `use SQL { ... }` e `use math.vec.dot as DOT;`.

#### Formatter
- **Suporte a Novas Estruturas da AST**: Completada a implementação do formatador (`FmtVisitor`) para garantir que os nós `SeqNode`, `WordCallNode`, `WordDeclNode`, `ContextDeclNode` e `UseNode` sejam corretamente reemitidos na saída gerada, sem serem descartados silenciosamente.

#### Testes e Cobertura
- **`tests/test-formatter.cpp`**: Novo arquivo de teste criado para verificar que o formatador preserva e reemite as estruturas AST experimentais corretamente.
- **Testes de Barreira em `test-sema`**: Adicionados casos de teste abrangentes cobrindo todos os diagnósticos e garantindo que as sintaxes experimentais falhem com o erro semântico E2010.
- **Testes de Scanner em `test-scan`**: Adicionados testes específicos para as sintaxes de declaração de contextos (`context`) e palavras-chave (`word`).
- **Regressão**: Atualizado o teste de sucesso unário de `!x` para `not x`, pois o operador `!` agora possui semântica de fallback rejeitada na barreira semântica.

---

### Ações Necessárias Futuras

#### Implementação Incremental de Semântica (Entrega 3)
- **Atribuição a Campos/Índices**: Definir e implementar a semântica de tipos, resolução, representação HIR e codegen para atribuição a campos e índices.
- **Chamadas Indiretas**: Implementar suporte robusto a chamadas de função indiretas em todo o pipeline.
- **Arrays e Indexação**: Implementar a tipagem, lowering e codegen completo para arrays e indexação.
- **Ativação de Sintaxes Experimentais**: Uma vez definidas suas representações HIR e semântica de tipos dedicadas, remover gradualmente a barreira semântica para:
  - Operadores `is` / `as`.
  - Operadores de propagação/fallback.
  - Sequências e chamadas de operadores customizados (`word`/`context`/`use`).

#### Regras de Integração para Agentes
- **Garantia de Não Sobreposição**: Manter a reserva de propriedade (ownership) por entrega (parser/AST, sema/HIR e formatter/testes) de forma que múltiplos agentes não editem os mesmos arquivos de subsistemas concorrentemente.
- **Validação de Código e Estilo**: Executar sempre `cmake --build build --target fmt` para manter a formatação do código em conformidade com o `.clang-format` antes de qualquer commit ou entrega.
