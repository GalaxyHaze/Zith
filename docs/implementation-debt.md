# Zith Implementation Debt

> Last updated: 2026-10-26.

Documento de gestão da dívida de implementação. Distingue propositadamente:

- **Dívidas reais**: funcionalidade que foi implementada de forma incompleta, com
  limitação conhecida que não é a intenção de design, ou código que repete lógica
  e dificulta manutenção.
- **Não-dívidas**: decisões de design intencionais para `Zith--`. Manter o
  comportamento atual, mesmo que pareça incompleto comparado com o spec maior do
  Zith.

Este ficheiro não substitui `docs/impl-status.md`. É o inventário de trabalho de
engenharia para rever e gerir.

---

## Release stdlib / installer follow-ups

- Os installers de release substituem o conteúdo de `<prefix>/share/zith/stdlib`
  antes de extrair, para evitar ficheiros stdlib antigos após um upgrade.
- Os smoke tests dos installers usam agora `examples/loops-simple.zith`, que
  exercita o compilador sem depender de libclang/C-header interop. A presença
  de `std/io/console.zith` continua a ser validada separadamente para cobrir
  descoberta e empacotamento da stdlib.
- O manifest `.github/scoop/bucket/zithc.json` foi regenerado para a release
  publicada `v0.6.3.2` do repositório canónico `GalaxyHaze/Zith-Lang`: os dois
  binários Windows e o ZIP da stdlib têm agora os quatro SHA-256 verificados
  contra os digests da release. O job `update-distribution` de
  `build-artifact.yml` repete esse cálculo automaticamente para cada release e
  valida o manifest em modo estrito. `update-package.yml` ficou apenas como
  reparação manual para releases antigas.
- Os shims Scoop executam `zithc` a partir de `~\scoop\shims`, fora do prefixo
  da app. O binário Windows resolve o caminho do módulo real com
  `GetModuleFileNameW`, e o smoke Scoop agora executa um `check` com
  `std/io/console` através do shim. `ZITH_STDLIB` e `--include` continuam
  disponíveis como overrides explícitos.
- O novo layout de `scripts/install.ps1` (`%LOCALAPPDATA%\Zith\bin` +
  `%LOCALAPPDATA%\Zith\share\zith\stdlib`) e o ramo MSYS/MinGW/Cygwin de
  `scripts/install.sh` têm agora smoke tests no `build-artifact`. Os installers
  também aceitam `ZITH_INSTALL_ROOT` e `ZITH_RELEASE_BASE_URL`, permitindo testar
  uma instalação isolada sem privilégios administrativos. O teste local Unix
  confirmou a descoberta automática da stdlib a partir do layout instalado.
  O repositório padrão dos três installers é agora
  `GalaxyHaze/Zith-Lang`, que é o destino canónico do rename; forks podem
  definir `ZITH_REPOSITORY` sem editar os scripts.
  Continua pendente a primeira execução real da matriz Windows, incluindo ARM.
- Em `.github/workflows/build-artifact.yml` os targets ARM64 usam runners
  nativos e pedem LLVM explicitamente; CMake falha se LLVM 18+ não estiver
  disponível, em vez de publicar um compilador sem codegen. Os quatro jobs
  Windows fornecem também o `LLVM_DIR` do pacote Chocolatey, porque o layout
  CMake do LLVM não pode depender apenas do `PATH` do runner.
- O CI regular (`ci.yml`) exige agora LLVM 18+ nos builds nativos Debug/Release.
  O `build-artifact` adicionou smoke tests de `install.sh`, `install.ps1` e
  Scoop, além da validação estrutural do manifest. O CI WASM continua a
  executar o runtime ABI. A cobertura de runners ARM e a instalação Scoop
  permanecem dependentes de uma execução de release real.
- O workflow `create-new-release.yml` publica os comandos de instalação a partir
  de `raw.githubusercontent.com/${{ github.repository }}/main/...`, alinhado
  com a branch atual. O mesmo workflow agora expõe a versão normalizada
  (`v` removido do input) como output do job de tag e usa esse output ao chamar
  `build-artifact`, evitando tags inválidas como `vv1.2.3` quando o operador
  fornece a versão com prefixo `v`.
- O bucket Scoop e a fórmula Homebrew são actualizados no job
  `update-distribution` de `build-artifact.yml`; ambos dependem de
  `RELEASE_PAT` e do tap externo. O Homebrew recebe a fórmula completa através
  da Contents API, protegida pelo SHA do ficheiro remoto, em vez de depender de
  um workflow remoto que só substitua texto parcial. O smoke de `scoop install`
  corre depois da atualização automática do manifest, usando o conteúdo de
  `main`. Os contratos estáticos de Scoop/Homebrew usam o identificador
  canónico `GalaxyHaze/Zith-Lang`. Os workflows usam `github.repository`, que o
  GitHub resolve para esse nome após o rename de `GalaxyHaze/Zith`.
- A fórmula Homebrew passa explicitamente o diretório CMake de
  `Formula["llvm"].opt_lib`, pois LLVM é keg-only e não deve ser descoberto por
  acaso através do `PATH`. A fórmula exige LLVM e essa dependência é agora
  verificável pelo contrato offline.
- O job `validate-release-assets` bloqueia a atualização de distribuição até
  todos os binários nativos, variantes musl, LSP, WASM e os dois formatos da
  stdlib existirem na mesma release. Assim, uma release parcial não pode
  produzir metadados Scoop/Homebrew aparentemente válidos nem sincronizar uma
  versão incompleta para o playground.
- A validação de assets deixou de ser o único gate de publicação: `update-distribution`
  e `sync-playground-wasm` esperam também os smoke tests dos installers Unix,
  Windows e WASM. O smoke Scoop continua depois do update do manifest, porque
  depende precisamente dos hashes/URLs que esse job gera.
- `create-new-release.yml` mantém a release como draft durante toda a construção.
  O job `publish-release` só remove o draft depois de assets, installers,
  metadados de distribuição, Scoop e sincronização do playground WASM passarem.
  Uma falha intermédia deixa uma release draft diagnosticável em vez de expor
  uma release incompleta.
- `scripts/validate-release-contract.py` verifica cada job nativo
  (`build-main`, `build-musl` e `build-lsp`) individualmente, exigindo
  `ZITH_HAS_LLVM=ON` e `ZITH_REQUIRE_LLVM=ON`, e confirma que o job WASM fica
  fora desse requisito. O SHA do archive Homebrew é calculado a partir de um
  download local com `curl --fail --retry`, não de uma pipeline que possa
  mascarar uma resposta truncada.
- `scripts/test-release-contract.py` executa os dois geradores contra assets
  temporários e confirma os quatro hashes Scoop, a versão/URL/SHA Homebrew e a
  validação estrita do manifest. O CI corre este teste sem acesso à rede.
- `scripts/verify-llvm-build.py` lê o `CMakeCache.txt` gerado e impede que um
  job publique um artefacto sem o backend LLVM efetivamente configurado. A
  versão detectada é persistida por CMake e precisa de ser 18 ou superior.
- Os builds musl deixaram de usar Zig sobre archives LLVM do host. Tanto o
  pipeline de release como o job `cross-builds` do CI constroem o compilador
  dentro de `alpine:3.22`, com o LLVM musl nativo (`clang20`, `llvm20-dev`,
  `llvm20-static`, `lld20`), `-DLLVM_DIR` apontado para essa instalação e
  `-DZLIB_USE_STATIC_LIBS=ON`. Ligar archives LLVM glibc do host a um alvo musl
  deixava símbolos `std::__cxx11` e `std::__once_callable` por resolver, porque
  o runtime C++ do link não coincidia com o runtime usado para construir o
  LLVM. O `scripts/verify-llvm-build.py` continua a verificar no `CMakeCache.txt`
  que o triple configurado coincide com o target musl esperado.
- O gate `validate-release-assets` também abre `zithc-wasm.zip` e exige
  `zith-playground.wasm` e `zith-stdlib.pack` não vazios antes de atualizar
  Scoop, Homebrew ou o playground. Essa regra vive agora em
  `scripts/validate-release-assets.py` e é exercitada pelo teste offline do
  contrato, evitando que a lista de assets do workflow e a validação do bundle
  WASM evoluam separadamente.
- `scripts/install-wasm.sh` aceita `ZITH_RELEASE_BASE_URL`, limpa o diretório
  anterior durante upgrades e falha se o bundle não trouxer o WASM ou o pack
  stdlib. O `build-artifact` executa agora um smoke test desse instalador.
- `update-package.yml` continua disponível apenas para reparar releases antigas,
  mas agora reutiliza `update-scoop-manifest.py` e
  `update-homebrew-formula.py`, com o mesmo cálculo fail-fast de SHA do fluxo
  automático. Os dois manifests são commitados juntos, e o workflow partilha
  com `build-artifact` um grupo de concorrência que serializa os pushes para
  `main`.
- `install.sh` e `install.ps1` fazem download e validação da stdlib em staging
  antes de substituir o binário ou a stdlib anterior. O caminho Unix foi
  testado para sucesso e falha sem deixar uma instalação parcial; a validação
  sintática/execução PowerShell permanece coberta pelo runner Windows, pois
  `pwsh` não está instalado no ambiente local.

---

## Não-dívidas (decisões de design)

| Item | Decisão |
|---|---|
| `const fn` | Não é pretendido em `Zith--`. O parser aceita `FunctionKind::Const`, mas o pipeline rejeita `const fn` com `UnsupportedSyntax` em [frontend-decl.cpp](/home/diogo/Zith/src/frontend/frontend-decl.cpp:406). Não documentar como dívida. |
| `dyn Interface` sem acesso a fields | O design expõe apenas métodos em `dyn`; fields ficam disponíveis em tipos concretos e bounds genéricos. `a.x on dyn Interface` com `E3001` é comportamento pretendido, não debt. |
| `type Name = T` cast-based | Em `Zith--`, `type Name = T` é nominal e o contrato explícito usa casts: `T as Name` constrói e `Name as T` extrai o campo subjacente. `Name` não é intercambiável com `T`; `alias Name = T` continua transparente. Sintaxe dedicada de construção/acesso é follow-up opcional, não é dívida activa. |
| Casts de utilizador | Não são pretendidos em `Zith--`. As conversões explícitas são os casts do spec (`as`, `raw as`), a coerção `opaque`, o narrowing `is` e os casts numéricos/ponteiro. Não existe trait de conversão (tipo `From`/`Into`/`Castable`) no spec nem na stdlib. Um novo branch de conversão em [classifyCast](/home/diogo/Zith/src/sema/sema-modern-utils.cpp:45) só seria adicionado se o spec de linguagem definir uma superfície dedicada. Não documentar como dívida. |
| `is <type>` fora de tagged unions e `opaque` | O Zith-- limita deliberadamente o narrowing de tipos a tagged unions e `opaque`. Testes gerais de tipo não fazem parte do subset. |
| NRA completa | Zith-- mantém a fatia SRA, congelada por design. A prova completa de ownership pertence ao full Zith e está especificada em [nra-spec.md](/home/diogo/Zith/docs/nra-spec.md). |
| C struct-by-value fora dos layouts validados | Não é dívida: a interop não promete todos os ABIs de agregados. Por valor, aceita apenas layouts integer-class documentados para x86-64/AArch64 Linux de 64 bits; outros targets e classes ficam deliberadamente fora do subset até serem validados. |
| Agregados VM com buffers geridos pelo heap | Esses tipos ainda não existem no subset da VM; `MemCopy` cobre os agregados actualmente suportados, sem prometer cópia profunda para tipos futuros. |
| `*char` em `InputLine.cast<T>` | Fica fora do contrato de parsing. Strings continuam disponíveis por `InputLine.text()`. |
| `..` / `...` como um token `Dots` | O lexer produz um único `TokenKind::Dots` para uma corrida de pontos ([ast-lowerer.cpp](/home/diogo/Zith/src/frontend/ast-lowerer.cpp:226)) e o parser distingue a grafia pelo lexema: `..` é o range ([frontend-expr-operator.cpp](/home/diogo/Zith/src/frontend/frontend-expr-operator.cpp:101)) e `...` é o marcador de variadic slice `[...]T` ([frontend-types.cpp](/home/diogo/Zith/src/frontend/frontend-types.cpp:115)). É um detalhe do lexer, não um defeito de comportamento. |

---

## Concluídas e reclassificadas

Estas entradas saíram do inventário ativo. Os identificadores históricos ficam
preservados para referências em planos, memória e testes.

| Referência | Resultado |
|---|---|
| `2. Cache .zirl` | Round-trips, hydration e divergência canónica estão cobertos em `test-cache`. |
| `3. NRA completa` | Reclassificada como trabalho de full Zith; o contrato Zith-- é SRA, congelado por design. |
| Formatter de `for` | Round-trip preserva `for (cond)`, `for {}`, labels e condições opcionais. `test-formatter` cobre reparse e idempotência. |
| `7. Falhas em test-codegen` | As seis falhas históricas foram corrigidas; a secção de regressões antigas foi retirada. |
| `8. ParseInput` | `InputLine.cast<T>` está implementado para os tipos suportados. A exclusão de `*char` é uma decisão de escopo, não uma dívida. |
| `11. Facades exportadas` | Prefixos partilhados, tipos qualificados e segmentos kebab-case estão cobertos pelo status de importação em `impl-status.md`. |
| `VM v2 #3, #5-#7, #10-#11` | Arena por frame, chamadas indiretas, cache HIR/artifact, diagnósticos JSON, variadic slice tails e lowering dos agregados suportados foram concluídos e cobertos pelos testes da VM. |
| Variadic tail logic | `VariadicCallPlan` é produzido em sema e consumido pelo HIR, sem rederivação local nos call sites. |
| Merge de `ProjectConfig` e options | Os call sites usam `session::mergeStrings`; já não há uma dívida separada de concatenação. |
| Canonicalização de `opaque` | `canonicalTypeId` e a mensagem `E2010` são partilhados. A identificação do field divergente continua no item 4. |
| Inicialização dos HIR nodes | Os `HirExpr` alternatives têm defaults para ids, tipos, flags e valores escalares. |

---

## Dívidas reais (features implementadas mas incompletas e código com risco)

### 3a. VM v2: inventário de dívidas e limitações abertas

Inventário revisto em 2026-10-18, cruzando os contratos assinados
([vm-v2.md](/home/diogo/Zith/docs/plans/vm-v2.md),
[0021](/home/diogo/Zith/docs/adr/0021-vm-v2-portable-execution.md),
[0024](/home/diogo/Zith/docs/adr/0024-wasm-vm-v2-abi.md),
[0025](/home/diogo/Zith/docs/adr/0025-vm-v2-call-convention-and-intrinsics.md))
com o estado de `src/vm/`. O opcode set já cresceu além do slice inicial
(bitwise, `IndexLoad`, `CallFnRef`/`CallRange`, `Branch2`). Contexto: a dívida
"duas fatias de execução" foi reaberta e resolvida de novo, o caminho nativo
sem-LLVM voltou (ver secção D) e `src/vm/` continua opcional via
`ZITH_BUILD_VM`; a VM v2 serve hoje o playground WASM, o harness `test-vm-v2` e
o `zithc run` quando o LLVM está off ou quando se passa `--virtual-machine`.
Os IDs numéricos são estáveis; itens concluídos ficam na tabela histórica
acima e só os itens ainda abertos permanecem neste inventário:

1. **Superfície de linguagem incompleta no lowering.** O slice de paridade
   Zith-- está coberto por testes focados e por casos pareados para optionals,
   enums numéricas, unions tagged e raw, `dyn` dispatch suportado, `opaque`
   simples, pipelines runtime e state-machine transfers. Isto não cobre toda a
   superfície aceita pelo compilador nativo. Dynamic traits fora do caso
   suportado, intrinsics e call targets fora do subset, literais não
   primitivos, índices de arrays dinâmicos e algumas formas de return ainda
   podem resultar em status 5. Um lowering HIR sem suporte falha explicitamente
   em vez de devolver um registo não atribuído que possa omitir stores ou
   returns.
2. **Computed-goto adiado.** A decisão registada é aplicar computed-goto ao
   dispatch loop "once the opcode set stabilizes". O opcode set ainda está a
   crescer (ver dívida 1), pelo que o loop atual é o dispatcher simples.
4. **FFI validado, alargado em 2026-10-10.** O subset é data-driven
   (`kValidatedExterns` em `src/vm/vm-v2.cpp`): `puts`, `putchar`,
   `write_stdout`, `malloc`, `calloc`, `free`, `realloc`, `snprintf`
   (`%u`/`%d`/`%g`), `printf` variádico, `strlen`, `memcpy`, `strncmp` e
   `getchar`. `calloc` e `strncmp` cobrem o caminho dos hash maps e dos
   parsers de input da stdlib. Um extern fora desta lista faz trap (VMV2-04)
   com uma mensagem que nomeia o extern em falta, pelo que a lista pode
   crescer com uma única entrada nova.
8. **Value width fixo.** Valores primitivos executam como `int64_t`/bits e fat
   pointers ocupam dois registos consecutivos. Decisão consciente do primeiro
   slice, a revisitar para larguras nativas e floats.
9. **Paridade stdlib WASM vs nativo.** A cobertura geral da stdlib via VM/WASM
   continua menor que via LLVM e cresce com as dívidas 1 e 4. Desde
   2026-10-18, `tests/test-stdlib-parity.cpp` compara doze programas in-scope
   pelos dois caminhos, incluindo stdout e exit code, e reporta fase e detalhe
   do VM. Os casos excluídos de console, allocator e hash map permanecem
   classificados em
   [vm-v2-stdlib-parity.md](/home/diogo/Zith/docs/vm-v2-stdlib-parity.md).
   Seis deles reportam atualmente status 5 durante VM lowering com
   `unsupported slot size in v2 lowering`. O harness só permite esse status
   com uma categoria de exclusão explícita. Trap, OOM e divergências de
   comportamento continuam a falhar.
### 4. Bare `opaque` mantém tags estáveis através de imports e cache

- Estado atual: o typeId canónico é derivado do namespace do módulo, ordem
  canónica de fields e nome do tipo. A regra vive numa única função partilhada,
  `types::canonicalTypeId` em
  [type-canonical.cpp](/home/diogo/Zith/src/types/type-canonical.cpp), usada
  tanto pelo lowering (`HirLowerModern::canonicalTypeId`, que apenas delega) como
  pela canonização do cache. A ordem canónica de fields (tamanho e depois nome)
  e o cálculo do tamanho de agregados também são partilhados
  (`types::typeByteCount`/`types::typeAlignBytes`), pelo que uma mudança do
  comparator deixa de ser uma lambda privada do lowering. O registry é o
  contract explícito de `canonical-any`: cada canonical id project-local recebe
  um runtime tag único numa cold build, o mapping é persistido e os artefactos
  serializam a mesma tabela em `canonical_mappings`. Na hydration warm, a tabela
  é validada contra o registry antes de re-hidratar `TypeIntern`; quando diverge,
  o `E2010` (mensagem única `cache::canonicalDivergenceMessage`) identifica a
  canonical id exacta e recomenda apagar `canonical-any` e os artefactos
  `.zirl`, depois reconstruir.
- Regra de evolução escolhida: mudanças de canonical field order alteram o
  canonical id e por isso não são re-mapeadas automaticamente. Uma build com
  canonização nova continua determinística, mas qualquer artefacto com um tag
  persistido antigo é rejeitado com o caminho de recuperação acima em vez de
  ser silenciosamente re-tagado. Sem mudança de canonical id, cold build,
  hydration warm, imports e valores `opaque` reutilizam o mesmo runtime tag.
- Dívida residual: o diagnóstico de divergência identifica a canonical id e
  recomenda a recuperação, mas ainda não identifica qual field originou a
  mudança. O registry project-local pertence ao compiler/cache e não exige um
  objecto no runtime do programa.
- `coerceValue` trata `opaque -> opaque` como no-op de sema, pelo que casts
  vindos de módulos importados não são rejeitados como erro de re-tagging.
- Referência: hydration e erro de instabilidade em
  [sema-cast-coerce.cpp](/home/diogo/Zith/src/sema/sema-cast-coerce.cpp),
  hydration em
  [persistent-cache.cpp](/home/diogo/Zith/src/session/persistent-cache.cpp:34)
  e testes em
  [test-cache.cpp](/home/diogo/Zith/tests/test-cache.cpp:347).

### 6. Narrowing numérico e de pointers continua incompleto

- Os casts `int -> int` estreitantes verificam em compile time valores
  constantes reconhecidos pelo sema. A adaptação implícita de literais (sem
  `as`) não verifica a faixa, e operandos variáveis ou conversões `float -> int`
  não têm checks de overflow em runtime.
- Narrowing de `?*T` continua limitado a provas flow-sensitive sobre bindings
  locais. Usos sem prova reportam `E3005`, e provas sobre expressões derivadas
  ou entre funções não são propagadas.
- O narrowing de optionais agregados (`?S`, `?pack`, `?i32`) passou a
  funcionar: depois de `if (x is null) { return ...; }`, os acessos `x.field` /
  `x.len` e `return x;` são aceites, como no nicho de `?*T`. O payload é
  extraído no local de uso (`TypedMap::optionalPayloadReads`), não no nome, para
  que `x?` continue a propagar. Ver `memory/sema-nonnull-narrowing.md`.
- Resolvido: um membro `null` num literal de pack. `inferPackLiteral` recebe o
  tipo de pack esperado da anotação (`let a: |bytes: ?*u8, len: u64| = |null,
  7u64|;`), e `null as ?T` fixa o tipo opcional de um `null` nu no call site
  (`new<Buffer>(|null as ?*u8, 0u64|)`). Um `null` sem contexto de tipo é
  rejeitado no próprio `inferPackLiteral` com `E3001`, pelo que o pack malformado
  nunca é internado. Antes a rejeição só existia no ramo de erradicação para
  `opaque`, e um binding direto (`let t = |null, 7u64|;`) chegava ao codegen com
  um membro `void` e pendurava o LLVM a 100% de CPU. Ver
  `memory/sema-nonnull-narrowing.md`.
- Resolvido: uma union genérica instanciada com `void`. O caminho de declaração
  já rejeitava um membro `void` ("union member cannot be 'void'"), mas a
  instanciação substituía `void` por um parâmetro de tipo (`union Pair<T> { T,
  i32 }` usado como `Pair<void>`, ou `Result<void, E>`), e esse membro chegava
  ao codegen e pendurava o LLVM em `getTypeAllocSize`. `instantiateTemplateFromArgs`
  (`src/sema/sema-type.cpp`, caso `DeclKind::Union`) passou a rejeitar um membro
  `void`/`never` com `E3001` no local de uso, com uma nota no membro genérico.
  `Result<void, E>` completo (uma representação unit para um membro) continua um
  item separado e maior. Ver `memory/sema-generic-union-void.md`.

### 9. HashMap genérico implementado e consumível entre módulos

`HashMap<K, V>` está implementado, com reificação estrutural, propagação de
bounds e persistência dos argumentos concretos no cache. O caminho de generics
importados (ADR 0020) também está fechado: `hm.HashMap<i64, i64>{}` reifica a
partir do template do módulo declarante, os defaults de campo são abaixados
nesse módulo, os métodos genéricos herdam os argumentos do recetor concreto, e
as chamadas genéricas aninhadas são concretizadas transitivamente
(`init<i64,i64>` chama `reserve<i64,i64>`). `examples/hash-map.zith` passa
`zithc check` e corre até código de saída 0, e o caminho é coberto por
`tests/test-generic-hashmap.cpp` e `test-codegen`. O segundo consumidor,
`DynArray<T>`, está promovido: `examples/dyn-array.zith` passa `zithc check` e
corre até código de saída 0, e `test-codegen` exercita um `DynArray<i32>`
importado de ponta a ponta.

Dívida residual: a cadeia de deref de `?*T` (o probe `entry-table.zith`, que usa
`self->table->occupied`) continua fora do caminho suportado pelo
`hash_map.zith`, que usa `raw self->table[...]`. Isto é um endurecimento do
sema de narrowing, não um bloqueio para a std-lib.

---

### 10. Conformance importada é determinística; trait defaults ainda usam scans globais

Estado atual: a causa raiz da instabilidade foi removida. A resolução de
declarações, tipos e métodos importados passou a ser feita através das
imports/bindings visíveis do módulo atual (`Import` e `ModuleAlias`) em vez de
percorrer todos os módulos carregados. `tests/test-interface-satisfaction.cpp`
agora cobre qualified calls sobre `InPlace` importado de `std/memory` num workdir
populado com vários módulos não relacionados.

Dívida residual de escopo: trait defaults e requisitos de `dyn Trait` ainda
são procurados em todos os módulos carregados quando o trait não está no
módulo atual. Isso já não escolhe o trait errado para o caminho aqui
reproduzido, mas deve ser estreitado quando houver uma definição exata de quais
defaults estão visíveis a partir do módulo de chamada.

Ação futura: formalizar o alcance dos trait defaults e remover os restantes
scans globais. Esta dívida é independente da resolução determinística de
conformance importada já coberta pelos testes.

---

### 12. Migrar o sink de formatação da stdlib para `TextSink`

O suporte a receivers mutáveis `lend dyn TextSink` está resolvido e coberto por
testes de sema e execução. A dívida restante é migrar
`Formatable.format(self, dest)` e as helpers de append de `lend FormatBuffer`
para `lend dyn TextSink`, incluindo `[]char` e buffers fixos. A intenção de
design está registada em
[0022-stdlib-io-format-split.md](/home/diogo/Zith/docs/adr/0022-stdlib-io-format-split.md)
e `CONTEXT.md`.

## Dívida de estrutura: monolitos

Os splits mecânicos de pipeline estão concluídos e não há nenhum split
aprovado ou ativo. A lista de unidades, responsabilidades e contagens atuais
`wc -l` está centralizada em
[docs/plans/monolith-splits.md](/home/diogo/Zith/docs/plans/monolith-splits.md).
O tamanho de um ficheiro é um sinal para revisão, não uma exigência automática
de refatoração.

O glob de `src/*.cpp` no CMake recolhe os novos ficheiros automaticamente. Depois
de criar ficheiros, é preciso reconfigurar (`cmake -S . -B build`) para o glob
ver os novos `.cpp`.

---

## Dívida estrutural residual

### Localização dos métodos extraídos em splits mecânicos

Os includes colados foram partidos, `isOpaquePointerCast` foi restaurado em
`sema-cast-coerce.cpp`, e os helpers de interface/resolução de `sema-modern.cpp`
foram movidos para `sema-type.cpp`. Resta rever a localização de cada método
quando a área voltar a ser alterada, para confirmar que corresponde à
responsabilidade do ficheiro.

### Contrato Homebrew depende de um tap externo não verificável localmente

O job [update-distribution](/home/diogo/Zith/.github/workflows/build-artifact.yml:309)
resolve a tag, calcula os hashes dos assets Windows/stdlib e calcula o sha256
do archive GitHub em
`https://github.com/GalaxyHaze/Zith-Lang/archive/refs/tags/v${version}.tar.gz`, depois
faz `PUT` da fórmula completa em `GalaxyHaze/homebrew-zithc` através da Contents
API. A fórmula recomendada fica documentada em
[zithc.rb](/home/diogo/Zith/.github/homebrew/zithc.rb): build a partir do source
tag com `-DZITH_HAS_LLVM=ON -DZITH_REQUIRE_LLVM=ON`, e stdlib instalada em
`share/zith/stdlib`, o caminho já lido por
[stdlib-discovery.cpp](/home/diogo/Zith/src/support/stdlib-discovery.cpp:107).

O archive e o SHA da release `v0.6.3.2` foram verificados localmente. O
workflow futuro recalcula esse SHA a partir do archive da tag e envia os mesmos
dados no dispatch.

Risco residual e validação em aberto:

- A fórmula atualmente publicada ainda não exige LLVM nem instala a stdlib no
  layout esperado. O `build-artifact` agora substitui a fórmula inteira na
  release seguinte, mas a convergência inicial e o build Homebrew real do tap
  continuam pendentes até uma execução autenticada do fluxo.
- O release atual não publica um binário macOS estável usado pela fórmula. A
  escolha defensável é build-from-source do archive de tag, sem inventar hash.
- A atualização usa `github.repository_owner`, portanto o tap é `GalaxyHaze`
  quando este repo o usar como remote. O workflow remoto antigo pode continuar
  no tap para compatibilidade, mas já não é a fonte de sincronização.

Acção recomendada: atualizar o tap externo para a fórmula LLVM-enabled desta
rama e confirmar o build Homebrew numa release real. Essa alteração fica fora
da work-tree deste repositório.

---

## Dívida de qualidade da code base (auditoria 2026-10-07)

Registo histórico da varredura estrutural de `src/` no baseline `935d7c00`,
realizada em 2026-10-07 com `cppcheck`, `clang-tidy` e inspeção manual. As
contagens pertencem a esse baseline, não à árvore atual. Nenhum dos itens era
uma decisão de design intencional. As secções A-D estão resolvidas, incluindo
a lacuna de execução VM descrita na secção D. O plano original foi arquivado
em
[codebase-quality-remediation-2026-10-07.old.md](/home/diogo/Zith/docs/plans/archive/codebase-quality-remediation-2026-10-07.old.md).
A auditoria atual está em
[codebase-audit-2026-10-15.md](/home/diogo/Zith/docs/codebase-audit-2026-10-15.md)
e o respetivo plano ativo está em
[codebase-quality-remediation.md](/home/diogo/Zith/docs/plans/codebase-quality-remediation.md).

### A. Referências a `src/ir` que já não existe (resolvida)

Estado resolvido: o slice de execução v1 está arquivado em
`archive/execution-ir-v1/` e `src/ir/` já não existe. O ADR público que o
descrevia como parte da árvore ativa foi corrigido para apontar para o arquivo:

- [0024-wasm-vm-v2-abi.md](/home/diogo/Zith/docs/adr/0024-wasm-vm-v2-abi.md:13)
  diz agora que o IR antigo está em `archive/execution-ir-v1/`.
Verificação: `rg -n "src/ir/(exec-ir|hir-to-ir)|src/interp/ir-vm"` nos
documentos públicos relevantes devolve zero correspondências.

### B. Ficheiro de merge residual em `src/` (resolvida)

Estado resolvido: `src/vm/typed-ir.hpp.orig` (um `.orig` de merge de 3520 bytes,
ignorado pelo `.gitignore` e por isso invisível no `git status`) foi removido.
Verificação: `find src tests -name '*.orig' -o -name '*.rej'` devolve zero
ficheiros.

### C. Membros sem default initializer que o analisador assinala (resolvida)

Estado resolvido: foi adicionado `memory::kInvalidInternedId` em
[string-interner.hpp](/home/diogo/Zith/src/memory/string-interner.hpp:15) e
usado como default nos dois campos assinalados:

- [hir-module.hpp](/home/diogo/Zith/src/hir/hir-module.hpp:22):
  `HirFunction::name` e `return_type` têm agora default
  (`memory::kInvalidInternedId` e `types::kInvalidType`), tal como os nodes de
  `hir-expr.hpp`.
- [options.hpp](/home/diogo/Zith/src/cli/options.hpp:276):
  `Options::subcommandArg` tem agora default. Continua a ser um campo sem
  leitor (o consumidor real é `subcommandStr`), mas já não pode ser lido com
  valor indeterminado.

Verificação: `cmake --build build -j4` limpo e `ctest --test-dir build` com
39/39 testes a passar.

### D. Duas fatias de execução por VM/IR sem teste de fronteira (resolvida)

Estado resolvido (ADR-0036 revisto): o ramo nativo sem-LLVM foi removido em
2026 e reintroduzido agora como fallback suportado. Em
[run.cpp](/home/diogo/Zith/src/cli/cmd/run.cpp) o CLI nativo escolhe a VM v2
quando `--virtual-machine` é passado em qualquer build, e automaticamente
quando o build não tem LLVM (`ZITH_HAS_LLVM` off) ou tem como alvo WASM. Um
build LLVM nativo sem a flag mantém o caminho link e exec. `--interpreted`
continua a ser o caminho do HIR interpreter.

`src/vm/` passou a ser uma fatia opcional no build nativo através da opção
`ZITH_BUILD_VM`; sem ela o macro `ZITH_HAS_VM` não é definido e `--emit-vir`
reporta um erro explícito em vez de baixar para a VM. O mesmo vale para
`--virtual-machine`, que falha com "this build excludes the VM v2 slice". O
`test-vm-v2` ganhou um guard de build e devolve `77` (convenção de skip do
CTest, registada com `SKIP_RETURN_CODE`), pelo que excluir a fatia o salta em
vez de partir a compilação.

A hidratação da cache persistente não restaura o `decl_id` (é um id de AST
local à sessão), pelo que o lowering da VM decide se uma função tem corpo
apenas por `blocks.empty()`. Sem esta regra, um `zithc run` nativo seguido de
`zithc run --virtual-machine` perdia o `main` hidratado e falhava com "the
typed IR has no main function". O `test-cli-commands` cobre esta regressão com
um run nativo seguido de um run pela VM.

Verificação: build LLVM com `ctest --test-dir build` a passar 42/42, build
`-DZITH_HAS_LLVM=OFF` a executar `zithc run` pela VM sem flag, e build
`-DZITH_BUILD_VM=OFF` a compilar sem `src/vm/` com `--virtual-machine` a
reportar erro explícito e o `test-vm-v2` a saltar.

### E. Falsos positivos verificados (não é dívida)

- `uninitMemberVarNoCtor` em `TypePtr::pointee`, `TypeDyn::target`,
  `TypeQualified::inner`, `Label::span`, `Trivia::kind`, `StructDef`,
  `EnumDef`, `UnionDef`, `ErrorInfo`, `Entry`, `FieldState`, `FmtFileResult`,
  `LineInfo` e `IntSpelling`: são structs de dados construídas por agregado em
  cada uso, nenhum campo é lido antes de escrito.
- [symbol-table.cpp](/home/diogo/Zith/src/symbols/symbol-table.cpp:248): o
  `invalidPrintfArgType_uint` para `%zu` com `size_t` foi verificado com
  `-Wformat=2 -Werror` (compila limpo), é falso positivo.

---

## Próximos passos para rever

1. A quebra de HIR, de `sema-modern.cpp`, de `frontend.cpp`, de
   `frontend-context.cpp`, de `compilation-session.cpp`, de `codegen-emit.cpp`,
   de `frontend-expr.cpp` e de `hir-lower-expr.cpp` está feita. Não há
   candidatos ativos na lista de monolitos. O contrato de execução para estes
   splits está em `docs/plans/monolith-splits.md`.
2. Em cada extracção, compilar `zithcLib` e correr os testes da área afectada.
   Use `ctest --test-dir build --output-on-failure` para regressões gerais.
3. Decisões deliberadas para Zith-- ficam em “Não-dívidas”. Reabri-las exige
   uma mudança no spec ou uma issue de produto separada.
4. Quando uma dívida for resolvida, removê-la do inventário ativo e preservar
   apenas uma referência curta nesta secção de conclusões, se ainda houver
   referências a acompanhar.
