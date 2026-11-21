# Plano: o comando `zithc test` e o teste nativo Zith--

## Objetivo

Transformar `zithc test` de um type-check em lote de ficheiros `.zithm` num
runner de testes nativo. O comando descobre funções `#[test]` sob `test_dir`,
compila cada ficheiro com um driver sintetizado pelo compilador, executa-o em
qualquer backend e reporta o resultado por teste, incluindo as localizações
das falhas. A iteração adiciona o atributo `#[test]` e os intrínsecos
`@assert`, `@check` e `@log` ao Zith--.

## Pré-condições

- `cwd` é `/home/diogo/Zith`.
- O build de desenvolvimento está configurado em `/home/diogo/Zith/build`.
- As fontes da biblioteca são descobertas por `file(GLOB_RECURSE)` no
  `/home/diogo/Zith/CMakeLists.txt`. Depois de adicionar `.cpp` novos, voltar a
  configurar o build para atualizar o glob.
- Preservar todas as alterações existentes no worktree. Não editar nem
  reverter alterações alheias a este plano.
- Não executar `cmake --build build --target fmt`, porque pode reformatar
  ficheiros sem relação com esta tarefa. Usar `fmt-check`.
- Não executar `git reset --hard`, `git checkout --` ou comandos que apaguem
  ficheiros do utilizador.
- A implementação de `test()` está em
  `/home/diogo/Zith/src/cli/cmd/tool.cpp`. O runner executa o driver sintetizado
  para cada ficheiro com testes e suporta codegen nativo, VM v2 e interpretador.
- A skill de execução é Sequenta apenas se o utilizador aprovar explicitamente
  antes do trabalho começar. Sem aprovação, executar de forma linear normal.

## Estado atual

- `zithc test` descobre ficheiros `.zithm` sob `test_dir`, executa os testes
  pelo driver sintetizado, aplica `--filter <substring>` e reporta cada
  resultado com as localizações das falhas de check e do primeiro assert
  falhado. Ficheiros sem testes são saltados; diretórios vazios e filtros sem
  correspondência são erros.
- O runner suporta codegen nativo, VM v2 e interpretador HIR. O CTest
  `test-zith-test-suite` verifica a fixture completa em native e VM v2. No HIR,
  verifica os limites de log e fixtures com `@assert`, `@check` e `@log`.
- `std/test` fornece helpers `assert*`, `fail` e helpers acumuláveis `check*`.
  As macros avaliam os argumentos uma vez e expandem `@assert` ou `@check` no
  contexto da função de teste. `assertOneOf` e `checkOneOf` percorrem arrays e
  slices de valores esperados. As mensagens formatadas usam
  `FormatBuffer` e `@log`; `assertNear` compara distância absoluta sem formatar
  floats e converte `actual`, `expected` e `tolerance` para `f64`.
- `@log` emite blocos delimitados em stdout. O runner captura o output do
  processo, analisa os blocos, oculta logs de testes aprovados por omissão,
  mostra-os em `--verbose` e limita o corpo acumulado a 4 KiB por teste. O
  `TestOutputBuffer` usa um único array fixo de 4 KiB para o corpo do log ativo
  e aplica o limite incrementalmente durante a captura.
- `test_dir` já está documentado em `docs/19-project-config.md`, sob
  `[paths]`, com o valor por omissão `tests`.
- `docs/Zith--.md` documenta `#[test]`, `@assert`, `@log` e os helpers
  disponíveis em `std/test`.
- A secção `Debug` do ZIRL regista `(teste, id de assert) -> ficheiro:linha:coluna`
  para o relatório do runner.

## Bloqueios de aceitação

Verificação em 2026-10-28:

- `test-zith-test-suite` passa e verifica a fixture completa de `std/test` em
  native e VM v2. No HIR, verifica truncagem de logs e uma fixture portátil
  com `@assert` e `@log`; a fixture rica não é executada nesse backend por
  enquanto.
- `./build/zithc --include stdlib test tests/zith --no-cache --verbose` e
  `./build/zithc --include stdlib --virtual-machine test tests/zith --no-cache
  --verbose` produzem `6 passed; 10 failed; 16 total`, com exit code `1` pelas
  dez falhas intencionais. A fixture inclui os helpers de igualdade, pertença,
  seleção entre valores, opcionais e tolerância float. O teste de integração
  compara stdout e stderr completos entre os dois backends.
- A tentativa de `./build/zithc --include stdlib --interpreted test tests/zith`
  falha no caminho rico de formatação: `user_defined_formatable` falha e as
  falhas de `assertEq`/`fail` não preservam o texto formatado. O diagnóstico
  adicional desse backend foi adiado a pedido do utilizador em 2026-10-27.
- Em 2026-10-29, `@check` passou no teste focado e na suite completa em
  native, VM v2 e HIR: falhas acumulam, o corpo continua e `@assert` posterior
  continua fail-fast.
- Os testes cobrem filtragem, logs ocultos/visíveis, texto de log que imita o
  protocolo de resultados, diretórios vazios, filtros sem correspondência,
  ficheiros ignorados, localizações, traps e a truncagem do buffer de logs.
- A aceitação integral nos três backends permanece em aberto. A fixture
  completa não foi reduzida; native e VM exercitam-na, enquanto HIR conserva
  cobertura portátil e de logs até o caminho rico de formatação ser suportado.
- **Concluído, com implementação no runner.** O `TestOutputBuffer` mantém um
  array fixo de 4 KiB para o corpo ativo e um contador agregado por teste, que
  limita também várias chamadas `@log` no mesmo teste. A mesma implementação é
  usada na captura nativa, no VM e no interpretador HIR. Os testes cobrem
  truncagem entre chamadas e reinício da capacidade no teste seguinte. O buffer
  não é sintetizado como armazenamento HIR no driver.

## Decisões de produto

- A unidade de teste é uma função anotada `#[test]`, não um ficheiro nem um
  módulo. Vários testes cabem no mesmo ficheiro.
- Passar significa compilar, executar e satisfazer as asserções em runtime. O
  comando deixa de ser um type-check.
- A descoberta é por ficheiro: cada `*.zithm` sob `test_dir` compila e corre
  sozinho, com o seu próprio driver e o seu próprio processo. `#[test]` só é
  descoberto no próprio ficheiro, nunca em módulos importados. Um ficheiro sem
  `#[test]` é saltado com uma nota, não é erro.
- O driver é sintetizado como HIR pelo compilador, depois do sema, para servir
  codegen nativo, VM v2 e interpretador pelo mesmo caminho.
- O resultado de cada teste vive numa tabela sintetizada pelo compilador com
  uma entrada por função `#[test]`. O valor é `0` quando nenhum `@assert`
  terminou o teste e `N` quando ele retornou no site de falha de id `N`.
  Falhas de `@check` são eventos separados e não terminam o teste. A tabela é
  infraestrutura do runner e não um global que o código de utilizador possa
  escrever.
- O driver continua depois de um teste falhar e preenche todos os slots. Um
  trap duro (segfault, divisão por zero) mata o processo e o runner reporta
  crash para esse ficheiro.
- `@assert(cond)` é fail-fast e `@check(cond)` acumula falhas dentro de
  `#[test]`. Ambos recebem uma condição booleana e não têm mensagem própria.
  Mensagens ricas pertencem à stdlib.
- Fora do contexto lexical de `#[test]`, ambos comportam-se como panic. Os
  helpers `check*` têm de expandir no ponto de chamada para herdarem esse
  contexto; funções comuns não o herdam.
- Um teste que falha num assert retorna de imediato. Os checks anteriores são
  preservados e o resto do corpo não corre.
- As funções `#[test]` são `void` e sem parâmetros. `#[test]` sobre uma função
  com parâmetros ou retorno não-void reporta `W2029 AttributeNotApplicable`.
- Um módulo de teste não pode declarar `main` e `#[test]` ao mesmo tempo. A
  colisão é erro.
- O `@log(msg)` acumula texto num buffer sintetizado por teste. Por omissão o
  driver imprime o log apenas dos testes que falham, e `--verbose` mostra o de
  todos. O próprio driver imprime o log, o runner apenas faz relay.
- Os backends suportados são todos: codegen nativo, VM v2 e interpretador.

## Modelo

### Zith test function

Uma função livre anotada `#[test]`, sem parâmetros e com retorno `void`. É a
unidade que o runner descobre e reporta. Não exige um `main` no módulo.

```zith
#[test]
fn adds_two_numbers(): void {
    @assert(1 + 1 == 2);
}
```

### Test driver

Um `main` sintetizado pelo compilador para um ficheiro de teste. Chama cada
função `#[test]` descoberta por ordem de declaração, limpa o buffer de log
antes de cada teste e, no fim, imprime o bloco de resultados. O driver é
construído como HIR, por isso o mesmo desenho serve os três backends.

### Test result table

Uma tabela com uma entrada por função `#[test]`. O valor `0` significa que
nenhum assert terminou o teste. O valor `N` significa que o teste retornou no
site de falha de id estático `N`. Falhas de `@check` são eventos separados e
não alteram o slot. A tabela é sintetizada pelo compilador, equivalente a um
global mutável privado, que a linguagem não permite escrever ao utilizador.

### Assertion intrinsic

`@assert(cond)` termina o teste na primeira condição falsa. Dentro de um teste,
baixa para `if (!cond) { table[i] = N; <fim do teste>; }`, onde `N` é o id
estático do site de falha. Fora de um teste, baixa para um panic.

### Check intrinsic

`@check(cond)` regista um evento com o ID do site quando a condição é falsa e
continua no bloco seguinte. O ID partilha a sequência estática dos sites de
`@assert`, permitindo localizar ambos com uma única tabela no `Debug` do ZIRL.
Fora de uma função `#[test]`, baixa para o mesmo panic que `@assert`. Os helpers
`check*` são macros para que o intrínseco expandido herde o contexto lexical
do teste chamador.

### Test log intrinsic

`@log(msg)` aceita um `[]char` já formatado e acumula-o no buffer do teste
corrente. O buffer tem capacidade fixa por teste (proposta: 4 KiB) com
truncagem, para evitar alocação dinâmica sintetizada nesta iteração. A
formatação rica é construída em `std/test` sobre o `FormatBuffer` existente.

### Location table

O compilador grava uma tabela `(teste, id de falha) -> ficheiro:linha:coluna`
na secção `Debug` do ZIRL, que hoje está reservada e vazia. O runner resolve o
slot lendo essa secção do artefacto que já tem em memória.

## Superfície de linguagem

- Novo valor `AttributeKind::Test` em `src/frontend/frontend.hpp`, aceite
  apenas no alvo `AttributeTarget::Function`.
- `classifyAttribute` em `src/frontend/attribute-classify.hpp` reconhece
  `test`. Os alvos ilegais continuam a usar `W2029`.
- `@assert`, `@check` e `@log` entram na lista de nomes de intrínsecos reconhecidos pelo
  frontend e no caminho de sema/HIR como os intrínsecos de layout atuais.
- `#[test]` sobre uma função com parâmetros ou retorno não-void é `W2029`.
- Um módulo com `main` e `#[test]` em simultâneo é erro de sema.

## Superfície de config

- Documentar `test_dir` em `docs/19-project-config.md`, sob `[paths]`, com o
  valor por omissão `tests`.

## Protocolo de saída

- O driver imprime o output do programa e os logs durante a execução, e só no
  fim imprime um bloco delimitado com a tabela de resultados. Cada linha do
  bloco tem a forma `zith-test <nome> <slot>`, onde `<slot>` é `0` ou o id do
  `@assert` que terminou o teste. Cada check falhado produz ainda um evento
  interno separado, removido pelo runner da saída visível.
- O runner lê o último bloco delimitado e faz relay do resto. O delimitador
  tem de ser improvável de colidir com texto do teste. O texto de log é
  impresso pelo driver e nunca é interpretado pelo runner, por isso não há
  colisão de parsing.
- O exit code do comando é `0` quando todos os testes passam e `1` quando algum
  falha. A localização do assert falhado é recuperada pela secção `Debug`.

## Passos

1. **Concluído.** Registar `AttributeKind::Test` e o reconhecimento de `test`
   em `classifyAttribute`, com `W2029` para alvos ilegais. Cobrir o caso de
   `#[test]` sobre função com parâmetros ou retorno não-void.

2. **Concluído.** Adicionar `@assert(cond)`, `@check(cond)` e `@log(msg)` como nomes de
   intrínsecos reconhecidos pelo frontend, com os tipos `bool` e `[]char`.

3. **Concluído.** `@assert` baixa para panic fora de teste e para escrita no
   slot dentro de teste. `@check` acumula falhas em eventos e continua dentro
   do teste, ou baixa para panic fora dele. `@log` emite blocos delimitados em stdout; o sink
   partilhado do runner limita o corpo acumulado a 4 KiB por teste.

4. **Concluído, com buffer no runner.** A tabela de resultados e o `main`
   driver por ficheiro estão sintetizados. O sink usa um array fixo de 4 KiB
   para o corpo ativo e um contador agregado por teste, em vez de armazenamento
   HIR sintetizado no driver.

5. **Concluído.** Gravar a tabela de localizações
   `(teste, id de falha) -> localização` na secção `Debug` do ZIRL e
   implementar a leitura correspondente.

6. **Concluído.** Reimplementar `test()` em `src/cli/cmd/tool.cpp` para
   compilar e executar cada ficheiro com o driver, capturar o output com o
   caminho `capture=true`, separar o bloco de resultados e imprimir o
   relatório por teste. Manter o `--filter <substring>` a casar contra o nome
   `modulo::fn`.

7. **Concluído.** Reportar por omissão uma linha por teste, com o nome, e um
   resumo final. Com `--verbose`, mostrar o log de todos os testes e o detalhe
   extra.

8. **Concluído.** Criar `std/test` com helpers fail-fast `assert*` e
   acumuláveis `check*`, todos macros no ponto de chamada. Usar o
   `FormatBuffer` existente para as mensagens.

9. **Concluído.** Atualizar `docs/19-project-config.md` com `test_dir`,
   `docs/Zith--.md` com `#[test]`, `@assert`, `@check` e `@log`, e a linha de
   estado em `docs/impl-status.md`.

10. **Concluído.** Registar o desenho no ADR `docs/adr/0045-*`.

## Testes e critérios de sucesso

- **Parcial.** `test-zith-test-suite` verifica a suite completa de `tests/zith/`
  em native e VM v2, além de cobertura portátil e de logs no HIR interpreter.
- **Concluído.** Falhas de check e assert reportam o nome do teste e as
  localizações corretas; o caso nativo também valida mensagens formatadas.
- **Concluído.** `test-test-output-buffer` verifica o limite agregado entre
  várias chamadas `@log` e a reposição da capacidade no teste seguinte.
- **Em aberto.** A suite completa ainda não produz o relatório esperado no HIR
  interpreter. Consultar os bloqueios acima.
- **Concluído.** Um ficheiro sem `#[test]` imprime a nota de salto e não falha.
- **Concluído.** `#[test]` sobre função com parâmetros ou retorno não-void
  reporta `W2029`.
- **Concluído.** `@assert(false)` fora de teste causa trap quando chamado por
  uma função de teste.
- **Concluído.** `@check(false)` fora do contexto lexical de teste causa trap
  quando chamado por uma função helper normal; `@check(1)` é rejeitado.
- **Parcial.** O build completo passou e o CTest passou 46/46. `fmt-check`
  continua a falhar em muitos ficheiros por divergências do clang-format local
  23.1.1; não há clang-format-18 instalado. O novo
  `tests/test-zith-test-suite.cpp` está formatado pelo formatter disponível.

## Fora de âmbito

- Isolamento por processo e um processo por teste. O MVP usa um driver por
  ficheiro.
- Captura de output por teste para os testes que passam. O `@log` cobre apenas
  o caso de mensagens explícitas.
- Crescimento dinâmico do buffer de log. O MVP usa capacidade fixa com
  truncagem.
- Formato TAP ou JUnit.
- Execução concorrente de ficheiros de teste. O comando mantém a ordem.
