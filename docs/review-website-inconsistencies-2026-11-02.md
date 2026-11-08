# Revisão de inconsistências documentais do site

Data: 2026-11-02
Estado: críticas por corrigir
Escopo: páginas Zith-- do repositório `Zith-website`, comparadas com a
especificação e o status do compilador neste repositório.

## 1. `global` aparece como funcional

**Prioridade: alta**

`docs/pages/zith--/reference.md:337` classifica `global` como binding válido
e documenta `E2010` como seu comportamento em caso de uso incorreto.
Entretanto, `docs/impl-status.md:74` também o lista em “Working”, enquanto
`docs/Zith--.md:879` diz que `global name: T = value` foi removido e reporta
`E2010`, e `docs/Zith---implementation.md:37` confirma que o parser rejeita
`global`.

**Recomendação:** remover `global` da lista de bindings funcionais na página de
referência. Corrigir também o status em `docs/impl-status.md` para não
classificar essa forma como funcional.

## 2. Terminologia inconsistente no pipeline de memória

**Prioridade: média**

`docs/pages/zith--/guide/memory-model.md:94` descreve a ordem como
`NTA/SRA`. O status atual chama a análise implementada em Zith-- de SRA
(`docs/impl-status.md:47`), mas a linha `docs/impl-status.md:101` ainda
atribui os factos residuais a NRA. No glossário, `NIA` é Numeric Interval
Analysis e `NRA` é Node Resource Analysis, ambos subsistemas futuros do ZPK;
`NTA` está listado como termo a evitar para NIA (`CONTEXT.md:732-740`).

**Recomendação:** usar SRA para a análise atualmente implementada em Zith-- e
separá-la claramente dos subsistemas futuros NIA e NRA. Rever a menção a NRA
em `docs/impl-status.md` para que não atribua à análise atual o nome do
subsistema completo futuro.

## 3. Código de erro incorreto para anotações de ownership

**Prioridade: média**

`docs/pages/zith--/reference.md:222` diz que `unique`, `share`, `belong` e
`mut` em posição de argumento reportam `E2007`/`E4007`. A especificação atual
identifica este diagnóstico como `E4007 InvalidCallOwnership`
(`docs/Zith--.md:784`). `E2007` é `NoMatchingFn`, conforme a tabela de
diagnósticos em `docs/impl-status.md:233`.

**Recomendação:** documentar apenas `E4007` para anotações de ownership
inválidas em chamadas.

## 4. Guia de sintaxe identifica a linguagem errada

**Prioridade: baixa**

`docs/pages/zith--/guide/syntax.md:11` começa por “Zith source”, embora a
página pertença ao guia de Zith--.

**Recomendação:** alterar a frase para identificar explicitamente o código
como fonte Zith--.
