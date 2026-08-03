# Wave 01 — Ship-Readiness Infrastructure

> Baseline: commit `bf7925e`. Cinco agentes executados **em paralelo**, com escopos mutuamente
> exclusivos. Nenhum agente depende do resultado de outro durante a execução.

## Objetivo da Onda

Fechar a infraestrutura de *ship-readiness* do compilador sem tocar na cadeia de linguagem
(`frontend` / `sema` / `hir-lower` / `formatter`), que está ocupada por WIP não commitado do
`when` pattern matching. No fim da onda: `.zirl` com secções reais e round-trip verificado,
`zithc test` e `zithc deps` funcionais, diagnósticos com qualidade de produção, um
`docs/roadmap.md` real como fonte de verdade para as ondas seguintes, e cobertura de testes para
os módulos de infraestrutura.

## Regra Obrigatória: NÃO COMPILAR

Nenhum agente desta onda corre `cmake`, `ninja`, `make`, `ctest`, `g++`/`clang++`, nem as
ferramentas que compilam (`cpp_sanitize`, `cpp_perf`, `cpp_bench`, `debug*`).

Motivo: os cinco agentes trabalham em paralelo sobre a mesma árvore de ficheiros. Qualquer build
apanha ficheiros meio escritos dos outros agentes e produz ciclos de erros falsos, gastando a
sessão a perseguir problemas que não são do próprio agente.

Verificação permitida, toda sem build:

- `cpp_check` no seu próprio diretório (análise estática, não precisa de build system).
- `cpp_nav` para diagnósticos e navegação.
- Leitura de código e revisão dos contratos deste documento.

A compilação e o `ctest` acontecem **uma única vez**, na integração final, feita pelo integrador
depois de os cinco agentes terminarem.

## Ficheiros Congelados (nenhum agente pode tocar)

| Ficheiro | Motivo |
|---|---|
| `src/frontend/frontend.cpp` | WIP `when` não commitado |
| `src/frontend/frontend.hpp` | WIP `when` não commitado |
| `src/frontend/frontend-printer.cpp` | WIP `when` não commitado |
| `src/sema/sema-modern.cpp` / `.hpp` | WIP `when` não commitado |
| `src/sema/hir-lower-modern.cpp` / `.hpp` | WIP `when` não commitado |
| `src/formatter/fmt-visitor.cpp` | WIP `when` não commitado |
| `tests/test-sema.cpp`, `tests/test-codegen.cpp` | Modificados fora desta onda |

Qualquer necessidade de alteração nestes ficheiros fica para a wave 02.

---

## Agente A — ZIRL Sections

**Escopo exclusivo:** `src/zirl/` (todos os ficheiros).

**Estado atual verificado:** `zirl-code-section.{cpp,hpp}`, `zirl-decl-section.{cpp,hpp}`,
`zirl-type-section.{cpp,hpp}` e `zirl-debug-section.{cpp,hpp}` existem com **0 linhas**; toda a
serialização vive monoliticamente em `zirl-writer.cpp` (266 l.) e `zirl-reader.cpp` (339 l.).

**Tarefas**

1. Extrair a serialização por secção dos métodos privados
   `Writer::writeMetadata/writeDecls/writeTemplates/writeCode` e
   `Reader::readMetadata/readDecls/readTemplates/readCode` para os ficheiros de secção vazios,
   um par `encode`/`decode` por secção.
2. Manter `Writer::write` e `Reader::read` como as únicas entradas públicas, com assinaturas
   inalteradas, delegando nas secções.
3. Preservar determinismo byte-a-byte: mesma ordem de secções, mesmo layout, e `kFormatVersion`
   **não** muda (nenhuma mudança de formato nesta onda).
4. Implementar `zirl-debug-section` como secção reservada válida: escreve payload vazio, lê e
   ignora, sem falhar o checksum.

**Inputs (read-only):** `src/cache/cache-types.hpp` (`Artifact`, `CompactType`, `CompactExpr`),
`src/zirl/zirl-header.hpp` (`SectionId`, `FileHeader`, `fnv1a32`), `src/zirl/zirl-buffer.hpp`.

**Outputs / contrato:** a API pública de `Writer`/`Reader` fica **byte-idêntica e
source-compatible** com hoje. Cada header de secção expõe exatamente um par, e nada mais:

```cpp
namespace zith::zirl {
bool encodeDecls(const cache::Artifact &artifact, ByteWriter &w);
bool decodeDecls(ByteReader &r, cache::Artifact &out);
// idem: encodeTypes/decodeTypes, encodeCode/decodeCode, encodeDebug/decodeDebug
}
```

**Critérios de conclusão:** ficheiros de secção não vazios; `writer.cpp`/`reader.cpp` reduzidos a
orquestração; `cpp_check` limpo em `src/zirl`; zero alterações a `src/cache/`.

---

## Agente B — Cache Hydration & Invalidation

**Escopo exclusivo:** `src/cache/` (todos os ficheiros) e `src/session/compilation-session.cpp`
**restrito** às funções `tryLoadPersistentCache()` e `writePersistentCache()`.

**Estado atual verificado:** `Store::load` funciona e `mCacheHydrated` já faz *short-circuit* de
várias etapas do pipeline, mas `cache-entry.cpp`/`cache-entry.hpp` estão vazios (1 l. / 0 l.) e a
hidratação descarta o artifact lido — `tryLoadPersistentCache` apenas conta decls/functions e
escreve uma linha de log.

**Tarefas**

1. Preencher `cache-entry.hpp/cpp` com o tipo que representa uma entrada hidratada em memória:
   artifact + fingerprint validado + estado de hidratação, e a lógica de validação que hoje está
   em `Store::validateArtifact`.
2. Fazer `tryLoadPersistentCache()` guardar a entrada hidratada no `CompilationSession` em vez de
   a descartar, para que os *short-circuits* já existentes de `mCacheHydrated` leiam dados reais.
3. Endurecer a invalidação: `Store::invalidate` segue os dependentes transitivos via `Manifest` e
   é idempotente sobre ciclos de dependência.
4. Expor as `StoreMetrics` (hits/misses/invalid/writes/evictions) de forma legível para o Agente C
   as consumir num comando CLI.

**Inputs:** `src/zirl/zirl-reader.hpp` e `zirl-writer.hpp`, consumidos **apenas pela API pública
atual**, que o Agente A garante inalterada. Não abrir nem editar ficheiros em `src/zirl/`.

**Outputs / contrato:**

```cpp
// src/cache/cache.hpp — assinatura existente, preservada
[[nodiscard]] StoreMetrics Store::metrics() const;

// novo, aditivo
[[nodiscard]] std::optional<cache::CacheEntry>
Store::loadEntry(std::string_view canonical_path, const session::ContentFingerprint &fp);
```

`Store::load` mantém-se para compatibilidade. O Agente C só depende de `metrics()`.

**Critérios de conclusão:** `cache-entry` implementado; hidratação persiste o artifact;
invalidação transitiva sem recursão infinita; `cpp_check` limpo em `src/cache`; nenhuma alteração
a `compilation-session.cpp` fora das duas funções nomeadas.

---

## Agente C — CLI: `test`, `deps`, `docs`

**Escopo exclusivo:** `src/cli/` (todos os ficheiros, incluindo `cmd/`).

**Estado atual verificado:** `test`, `docs` e `repl` são `stubCommand()` em
`src/cli/cmd/tool.cpp`; `deps` imprime apenas uma linha de usage. `repl` fica **fora** desta onda
porque precisa de avaliação incremental sobre o frontend congelado.

**Tarefas**

1. `zithc test <path>`: descobrir ficheiros `.zith` de teste sob o diretório do projeto, correr o
   pipeline em modo `check` reutilizando `commands::runOnFiles` + `countPassed`, e reportar
   passed/failed/total, com *exit code* não-zero se algum falhar.
2. `zithc deps list`: ler `ZithProject.toml` com `toml++` (padrão já usado em `tool.cpp`) e listar
   as dependências declaradas. `add/remove/publish/unpublish/update` continuam a reportar
   não-implementado, mas com mensagem por subcomando em vez de usage genérico.
3. `zithc docs`: emitir para stdout as declarações públicas dos ficheiros de entrada a partir do
   snapshot que o pipeline já produz. Sem geração de HTML.
4. Adicionar a flag `--cache-stats`, que imprime `cache::StoreMetrics` no fim de um `build`.

**Inputs:** `cli/commands.hpp` (helpers `collectFiles`, `runOnFiles`, `countPassed`),
`session::Stage`, e `cache::Store::metrics()` do contrato do Agente B — **apenas via header**, sem
editar `src/cache/`.

**Outputs / contrato:** as assinaturas em `cli/commands.hpp` ficam inalteradas
(`int test(const Options &)`, `int deps(const Options &)`, `int docs(const Options &)`). Campos
novos em `Options` só por *append*, com default que preserva o comportamento atual. Formato de
saída fixado: `[ok] N/M tests passed`.

**Critérios de conclusão:** os três comandos produzem saída útil e *exit codes* corretos; `repl`
continua stub declarado; `cpp_check` limpo em `src/cli`.

---

## Agente D — Diagnostics Quality

**Escopo exclusivo:** `src/diagnostics/` (todos os ficheiros).

**Estado atual verificado:** `emit.cpp` (194 l.) já faz `findLine` e label de severidade;
`Diagnostic` já carrega `labels` e `suggestions`, mas os códigos em uso são poucos (E0001, E0000,
E2002, E1006, E3003, W1008) e E0000 é um saco genérico para type mismatch e validação de
opcionais.

**Tarefas**

1. Alocar códigos dedicados em `error-codes.hpp` para as famílias hoje enterradas em E0000:
   incompatibilidade de tipos, falha de coerção, violação de opcional/null, e aritmética entre
   larguras diferentes. Reservar as gamas e documentar cada código no header.
2. Renderizar `labels` e `suggestions` em `emit.cpp` — hoje são armazenadas e nunca impressas.
   Label secundário com o seu próprio recorte de linha e caret; sugestões como linhas `help:`.
3. `findLine` faz uma varredura O(n) desde o início do ficheiro por cada diagnóstico. Trocar por
   uma consulta ao `memory::SourceMap` (já disponível), mantendo `findLine` como *fallback* quando
   não existe source map.
4. Garantir que a saída sem cor é byte-estável e parseável por máquina, no formato
   `path:line:col: severity[CODE]: message`.

**Inputs (read-only):** `memory::SourceMap`, `memory::SourceFile`, `cli/terminal.hpp`
(`ColorTheme`).

**Outputs / contrato:** os códigos existentes **não mudam de número nem de significado** (E0001,
E2002, E1006, E3003, W1008 preservados); só se acrescentam códigos novos. Os *call sites* fora de
`src/diagnostics/` continuam a compilar sem edição — a migração de E0000 para os códigos novos é
wave 02, porque esses call sites vivem nos ficheiros congelados. Este agente entrega os códigos e
o renderer, **não** os call sites.

**Critérios de conclusão:** labels e sugestões visíveis; formato de uma linha estável; tabela de
códigos documentada no header; `cpp_check` limpo em `src/diagnostics`.

---

## Agente E — Roadmap, Impl-Status & Testes de Infra

**Escopo exclusivo:** `docs/` (todos os ficheiros), `CHANGELOG.md`, `tests/` (**apenas os ficheiros
novos listados abaixo**) e — em exclusivo nesta onda — o root `CMakeLists.txt`.

**Estado atual verificado:** `docs/impl-status.md` está desatualizado: declara `when` como *parse
error* e não menciona `Range`, `Placeholder` nem `LayoutIntrinsic`, que já existem em
`frontend::ExprKind` e já têm `inferWhen`/`inferRange`/`lowerWhen`. Não existe `docs/roadmap.md`.

**Tarefas**

1. Criar `docs/roadmap.md` como fonte de verdade das ondas seguintes: derivar de
   `docs/Zith-spec.md` + capítulos 02–21 e da tabela "Spec Only" do impl-status, agrupando as
   *features* por onda candidata, com IDs estáveis (`F-01`, `F-02`, …) e as dependências entre elas
   explícitas.
2. Corrigir `docs/impl-status.md` contra o código real, **por leitura de código e não por
   execução** do compilador: marcar `when`/`Range`/`Placeholder`/`LayoutIntrinsic` como *em
   progresso (não commitado)* em vez de *parse error*. Substituir a nota "verified against
   `build/zithc`" por "verified against source at `bf7925e`", já que esta onda não compila.
3. Escrever testes novos, um ficheiro por agente-alvo, **sem tocar nos existentes**:
   `tests/test-zirl-sections.cpp` (round-trip por secção, rejeição de truncagem e de checksum
   inválido), `tests/test-cache-entry.cpp` (hidratação e invalidação transitiva),
   `tests/test-cli-commands.cpp` (parsing de opções e *exit codes*),
   `tests/test-diagnostics-render.cpp` (formato de uma linha, labels, sugestões).
4. Registar os quatro testes no root `CMakeLists.txt` com `add_zith_test`, imediatamente após
   `add_zith_test(test-optional-slice tests/test-optional-slice.cpp)`.
5. Acrescentar ao `CHANGELOG.md` uma secção nova para esta onda, sem reescrever entradas
   anteriores.

**Inputs:** os contratos declarados pelos agentes A–D neste documento. Os testes são escritos
**contra os contratos**, não contra a implementação — é por isso que este agente não espera por
ninguém.

**Outputs / contrato:** `docs/roadmap.md` com IDs de *feature* estáveis reutilizáveis pelas ondas
seguintes; quatro executáveis de teste registados no CTest; nenhuma edição a ficheiros `tests/*`
pré-existentes nem a qualquer ficheiro em `src/`.

**Critérios de conclusão:** roadmap existe; impl-status coincide com o código; quatro testes novos
registados; changelog atualizado.

---

## Contratos de Integração

| De → Para | Contrato | Porque não bloqueia |
|---|---|---|
| A → B | `zirl::Writer::write` / `zirl::Reader::read` congelados nesta onda | B consome só o header; A não muda assinaturas nem formato |
| B → C | `cache::Store::metrics()`, já existente | C lê o header, nunca o `.cpp` |
| D → todos | Códigos atuais mantêm número e significado; só *append* | Nenhum call site fora de `src/diagnostics/` precisa de mudar |
| E → A/B/C/D | Testes escritos contra os contratos acima | Se um teste falhar na integração, o contrato deste documento é a fonte de verdade |

Sobre o CMake: todos os `.cpp` de `src/` entram por `GLOB_RECURSE`, logo A–D **não** precisam de
tocar no CMake para adicionar ficheiros de implementação. Apenas E edita o root `CMakeLists.txt`,
e só para registar testes.

## Riscos de Conflito e Como Foram Evitados

| Risco | Mitigação |
|---|---|
| Cadeia da linguagem com WIP não commitado | Os ficheiros afetados estão congelados; nenhuma tarefa desta onda os requer |
| Root `CMakeLists.txt` é o único registo de testes | Propriedade exclusiva do Agente E; A–D beneficiam do `GLOB_RECURSE` |
| `src/zirl` e `src/cache` fortemente acoplados | API pública do zirl congelada: A refatora o interior, B só consome o header |
| `compilation-session.cpp` é orquestrador partilhado | B está restrito a duas funções nomeadas; nenhum outro agente tem esse ficheiro no escopo |
| Códigos de diagnóstico usados em toda a árvore | Regra *só append*: D não renumera nem migra call sites |
| Builds concorrentes a corromper-se mutuamente | Compilação proibida durante a onda; um único build na integração |

## Ordem de Merge / Integração Final

1. **A (zirl)** — mais interior; nada depende do seu interior.
2. **B (cache)** — assenta sobre a API zirl inalterada.
3. **D (diagnostics)** — independente, mas antes do CLI para que a saída de C já use o renderer novo.
4. **C (cli)** — consome `metrics()` de B e o renderer de D.
5. **E (docs/tests/cmake)** — último, para que os testes registados corram contra tudo integrado.

Depois do merge dos cinco, e só então, o integrador corre uma vez:

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --build build --target fmt-check
```

Falhas de compilação nesta fase são resolvidas pelo integrador com a regra: **o contrato deste
documento vence a implementação**. O WIP do `when` continua não commitado e intocado no fim da
onda.

## Pressupostos e Defaults Escolhidos

- Não existia `docs/roadmap.md`; `docs/impl-status.md` foi tratado como o roadmap de facto, e o
  Agente E cria o roadmap real.
- O WIP do `when` fica não commitado e por integrar; é o primeiro item da wave 02.
- `zithc repl` fica fora desta onda: precisa de avaliação incremental sobre o frontend congelado.
- `kFormatVersion` do zirl mantém-se em `2` — a refatoração das secções é puramente interna, sem
  mudança de formato on-disk.
- A migração dos call sites de E0000 para códigos dedicados fica para a wave 02, porque esses call
  sites vivem nos ficheiros congelados.
- LLVM é opcional no build e nenhuma tarefa desta onda depende de `ZITH_HAS_LLVM`, logo a onda
  valida igualmente numa árvore sem LLVM.
