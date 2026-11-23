# Spec: Blocos Nomeados e `@marker`

> Rascunho de issue para o tracker. Publicar com `gh issue create` e aplicar a
> label `ready-for-agent`.

## Problem Statement

O corpo de uma `fn` ou `state` longa em Zith-- é hoje uma sequência anónima
de statements. Quem lê não vê as fases da função (setup, render, teardown)
sem as extrair para funções auxiliares, e quem escreve não tem forma de
anexar cleanup gerido a uma dessas fases: ou repete `enter`/`leave` à mão, ou
regista `defer` manualmente para cada recurso, com a ordem de saída e a
higiene dos guards à sua responsabilidade. O erro típico é esquecer o `leave`
num caminho de saída ou inverter a ordem de libertação de recursos
emparelhados.

## Solution

Duas formas novas de statement, válidas apenas diretamente no corpo de uma
`fn` ou `state`:

```zith
fn main() {
    setup: {
        // escopo lexical normal, nome apenas descritivo
    }

    render: @marker(renderer, log) {
        renderer.draw();
        log.write("frame");
    }
}
```

O bloco nomeado simples cria um escopo lexical normal; o nome não tem efeito
runtime e serve leitura, tooling e diagnósticos. A forma com `@marker` recebe
argumentos que são place-expressions a implementar o trait `Scope`; a macro
gera `guard.enter()` por argumento, da esquerda para a direita, e
`guard.leave()` por ordem inversa, através da infraestrutura de `defer`, para
qualquer caminho de saída do bloco. Os guards são bindings ocultos e
higiénicos. Em pipelines, um estágio pode ser um bloco nomeado
(`step1: { } |> step2: { }`), tratado como um estágio normal com todas as
regras de pipeline.

## User Stories

1. Como autor de uma `fn` longa, quero dividir o corpo em blocos nomeados, de
   modo a tornar as fases da função visíveis sem extrair funções auxiliares.
2. Como autor, quero que um bloco nomeado crie um escopo lexical próprio, de
   modo a que bindings locais da fase não poluam o resto do corpo.
3. Como autor, quero declarar `setup: { ... }` sem custo runtime, de modo a
   documentar a fase sem pagar por ela.
4. Como autor, quero anexar `@marker(renderer, log)` a um bloco nomeado, de
   modo a obter cleanup gerido dos dois recursos sem escrever `defer` à mão.
5. Como autor, quero que `enter` corra da esquerda para a direita sobre os
   argumentos, de modo a adquirir recursos na ordem em que os declarei.
6. Como autor, quero que `leave` corra por ordem inversa à de `enter`, de
   modo a libertar recursos emparelhados na ordem correta.
7. Como autor, quero que o cleanup corra em fallthrough, `return` e exits
   labelados que saiam do bloco, de modo a não fugir com recursos por limpar.
8. Como autor, quero passar um binding local como argumento de `@marker`, de
   modo a gerir um recurso criado antes do bloco.
9. Como autor, quero passar um parâmetro da função como argumento, de modo a
   gerir um recurso recebido do chamador.
10. Como autor, quero passar um global como argumento, de modo a gerir estado
    partilhado do módulo.
11. Como autor, quero passar um campo (`self.renderer`) como argumento, de
    modo a gerir um recurso guardado num agregado.
12. Como autor, quero que um temporário como argumento seja rejeitado, de
    modo a não prender um guard a um valor que não tem dono estável.
13. Como autor, quero que um argumento sem implementação de `Scope` seja
    rejeitado com diagnóstico dedicado, de modo a perceber que falta o
    `implement`.
14. Como autor, quero escrever blocos nomeados dentro de `state`, de modo a
    estruturar estados longos como estruturo funções.
15. Como autor, quero escrever `step1: { } |> step2: { }`, de modo a nomear
    estágios de pipeline sem perder as regras de `it`.
16. Como autor de pipeline, quero que um estágio nomeado consuma `it`
    exatamente uma vez por caminho, de modo a manter o contrato de consumo de
    `|>`.
17. Como autor, quero que `return`, `jump` e `break`/`continue` para loops do
    próprio estágio continuem válidos num estágio nomeado, de modo a que o
    controlo de fluxo local não mude por o estágio ter nome.
18. Como autor, quero um erro claro ao escrever um bloco nomeado dentro de um
    `if`, de modo a aprender a regra de posição no primeiro uso.
19. Como autor, quero o mesmo erro dentro de loops, blocos anónimos e outros
    blocos nomeados, de modo a que a regra seja uniforme fora de pipelines.
20. Como autor, quero que nomes duplicados no mesmo corpo sejam rejeitados,
    de modo a que cada fase tenha um nome único e pesquisável.
21. Como autor, quero que um bloco nomeado que colida com um label de loop do
    mesmo corpo seja rejeitado, de modo a não haver dois significados para o
    mesmo nome.
22. Como autor, quero que `break setup;` para um bloco nomeado seja
    rejeitado, de modo a que labels de loop e nomes descritivos não se
    confundam.
23. Como implementador de um tipo de recurso, quero escrever
    `implement Scope for Renderer` com `fn enter(var self): Guard`, de modo a
    definir o meu próprio efeito de entrada, como um checkpoint de render.
24. Como implementador, quero definir o tipo de `Guard` e o seu
    `fn leave(var self)`, de modo a escolher o efeito de saída, como flush de
    log.
25. Como implementador, quero que falhas internas de `enter`/`leave` se
    registem no stream em vez de falhar a operação, de modo a que o cleanup
    gerido seja infalível na superfície da linguagem.
26. Como autor de macros, quero marcar uma macro da stdlib com `#[scope]`, de
    modo a permitir a posição outer `@macro(...) { }` sem nomes hardcoded no
    compilador.
27. Como autor, quero que uma macro sem `#[scope]` na posição outer seja
    rejeitada, de modo a que a posição seja reservada a macros scope-aware.
28. Como mantenedor de tooling, quero os nomes dos blocos preservados no HIR,
    de modo a mostrá-los em diagnósticos e no IDE.
29. Como consumidor da ABI, quero que os nomes não façam parte da ABI, de
    modo a poder renomear blocos sem quebrar artifacts compilados.
30. Como leitor de diagnósticos, quero que erros dentro de um bloco nomeado
    mencionem o nome do bloco, de modo a localizar a fase com problemas.

## Implementation Decisions

- Duas formas de statement: `nome: { ... }` (bloco nomeado simples) e
  `nome: @macro(args) { ... }` (bloco nomeado com marcador). O nome é um
  identificador descritivo seguido de `:`.
- Regra de posição: válidos apenas como statement direto do corpo de `fn` ou
  `state`. Rejeitados dentro de `if`, loops, blocos anónimos e outros blocos
  nomeados. Exceção: um estágio de pipeline pode ser um bloco nomeado, com
  todas as regras de pipeline intactas (`it` consumido uma vez por caminho em
  `|>`, observação read-only em `also`, controlo de fluxo local ao estágio).
- Semântica do bloco simples: escopo lexical normal. O nome não tem efeito
  runtime, não introduz cleanup e não é alvo de controlo de fluxo.
- Semântica do marcador: cada argumento deve ser uma place-expression
  (binding, parâmetro, global ou campo) cujo tipo implementa o trait `Scope`.
  Temporários são rejeitados. A expansão gera `enter` por argumento da
  esquerda para a direita e regista `leave` por ordem inversa através da
  infraestrutura de `defer`, correndo em todos os caminhos de saída do bloco.
- Guards higiénicos: o resultado de cada `enter` fica num binding oculto
  gerado pela macro, inacessível ao código do utilizador e sem colisão com
  nomes do corpo.
- Infalibilidade: `enter` e `leave` não falham na superfície da linguagem;
  falhas internas registam-se no stream da implementação.
- Trait na stdlib: `Scope` declara `fn enter(var self): Guard`; o `Guard`
  resultante declara `fn leave(var self)`. Cada implementação escolhe o seu
  tipo de guard e o efeito (checkpoint de render, flush de log, etc.).
- Atributo `#[scope]`: `@marker` é uma macro normal da stdlib marcada com
  `#[scope]`. Só macros com esse atributo podem aparecer na posição outer
  `@macro(...) { }`. O compilador não contém nomes de macros hardcoded para
  esta posição.
- Unicidade de nomes: nomes de blocos nomeados são únicos por corpo de
  `fn`/`state`; duplicação é erro, incluindo colisão com labels de loop do
  mesmo corpo. Nomes de blocos nomeados nunca são alvo de `break`/`continue`.
- HIR: o nome do bloco é preservado no nó HIR correspondente para tooling e
  diagnósticos, à semelhança de `HirLoopTarget` carregar o label do loop.
  Nomes não entram na ABI nem no formato ZIRL.
- Diagnósticos dedicados: três novos, posição inválida, argumento que não
  implementa `Scope` (incluindo temporários), e nome duplicado. Seguem o
  formato e registo dos códigos existentes (família `E2010` e afins).
- Módulos tocados: parser (novas formas de statement e atributo `#[scope]`),
  expansão de macros (posição outer), sema (validação de posição,
  place-expressions, conformidade `Scope`, unicidade de nomes), lowering HIR
  (nó de bloco com nome, emissão de enter/leave via defer).
- Documentação: novo ADR em `docs/adr/` (próximo número livre, 0064), mais
  atualizações a `docs/Zith--.md`, `docs/09-control-flow.md` e
  `docs/Zith---implementation.md`. Se o design aceite divergir do ADR-0055 em
  cleanup gerido, o novo ADR diz-o explicitamente em vez de o sobrepor em
  silêncio.
- Ordem de implementação: parser, sema, HIR, testes, nesta ordem.

## Testing Decisions

Um bom teste verifica comportamento externo (diagnóstico emitido, ordem
observável de `enter`/`leave`, nome presente no HIR) e não detalhes internos
da expansão. As seams já existem e chegam; não são propostas seams novas.

- Parser e expansão de macros: seam `frontend::parse` com inspeção de
  snapshot e diagnósticos, o padrão de `tests/test-macro.cpp` e
  `tests/test-frontend.cpp`. Cobre as duas formas, o atributo `#[scope]`, a
  posição outer, e os três diagnósticos dedicados (posição inválida dentro de
  `if`/loop/bloco anónimo/bloco nomeado, argumento temporário ou sem
  `Scope`, nome duplicado e colisão com label de loop).
- Sema e lowering: conformidade de `Scope` por posição de argumento (binding,
  parâmetro, global, campo), rejeição de temporários, e estágios nomeados em
  pipeline com as regras de `it`. Prior art:
  `tests/test-frontend-modern-pipeline.cpp`.
- Ordem runtime de enter/leave: seam compile-link-run de
  `tests/test-codegen.cpp` (o helper `t.run`, já usado para provar a ordem
  LIFO de `defer` via `putchar`). Um tipo de teste com `Scope` cujo
  `enter`/`leave` emitem marcadores observáveis prova ordem de entrada
  esquerda-para-direita, saída inversa, e execução em fallthrough e `return`.
- Preservação do nome no HIR: inspeção do módulo HIR em
  `tests/test-hir-lower-modern.cpp`.
- Corpus end-to-end: ficheiro `.zithm` em `tests/lang/tests/` a exercitar
  blocos nomeados via CLI real, o padrão de
  `tests/lang/tests/control_flow.zithm`.
- Documentação: `tests/test-docs.cpp` valida os exemplos novos em
  `docs/09-control-flow.md` se cobrir esse ficheiro.

## Out of Scope

- `break`/`continue` com blocos nomeados como alvo. Nomes são descritivos,
  nunca labels.
- Blocos nomeados aninhados, ou dentro de `if`, loops ou blocos anónimos. A
  exceção de pipeline é a única posição composta.
- Cleanup automático genérico ou um hook `drop` ao nível da linguagem.
  `@marker` é uma macro da stdlib, não uma feature de ownership.
- Falha de `enter`/`leave` na superfície da linguagem. Falhas internas vão
  para o stream da implementação.
- Aplicar marcadores a statements que não blocos nomeados, ou fora do corpo
  de `fn`/`state` e estágios de pipeline.
- Conjunto inicial de tipos da stdlib a implementar `Scope`, além do mínimo
  necessário aos testes.
- Mudanças de ABI ou de formato ZIRL. Os nomes não são serializados.

## Further Notes

- A regra de posição existe para manter os nomes ao nível das fases de uma
  função, e não como mecanismo geral de estruturação de código aninhado.
- O pipeline `step1: { } |> step2: { }` não é uma exceção semântica: o bloco
  nomeado é o estágio, e as regras de `|>`/`also` aplicam-se sem alteração.
- A unicidade de nomes partilha o espaço com labels de loop no mesmo corpo
  para que um identificador tenha sempre um único significado, mesmo que os
  mecanismos sejam distintos.
- Relação com ADR-0055: o ADR regista que Zith-- não tem cleanup automático e
  que `defer` é explícito. `@marker` mantém essa letra (o cleanup é a macro a
  emitir `defer`) mas muda a ergonomia; o novo ADR deve registar esta tensão
  e a decisão.
- Publicação: criar a issue com este corpo via
  `gh issue create --title "..." --body-file docs/specs/named-blocks-marker-spec.md`
  e aplicar a label `ready-for-agent`. Título proposto:
  `feat: blocos nomeados e @marker com cleanup gerido (Zith--)`.
