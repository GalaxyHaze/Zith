# Revisão de inconsistências documentais do site

Data: 2026-11-02
Estado: corrigidas (revisão de 2026-11-24)
Escopo: páginas Zith-- do repositório `Zith-website`, comparadas com a
especificação e o status do compilador neste repositório.

Revisão de 2026-11-24: cada item foi reauditado contra o compilador e o site
atuais. Os quatro estão corrigidos. As correções estão no commit
`92161c1` do `Zith-website` e no commit `04a6ccbb` deste repositório.

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

**Resolvido (2026-11-24).** `zithc check` rejeita `global name: i32 = 5` com
`E2010: Zith--: 'global' is not supported; use 'const NAME: T = value'`. A
página `reference.md` já listava a forma em "Removed restrictions", e o
`docs/impl-status.md` foi corrigido para `let`, `var`, `const` (sem `global`).
O guia `bindings.md` deixou de dizer que a tabela de status ainda lista
`global` como funcional.

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

**Resolvido (2026-11-24).** `docs/impl-status.md` passou a dizer "SRA residual
facts are attached before HIR" na linha dos qualificadores de memória, e o
guia `memory-model.md` usa `sema -> comptime/solve -> SRA -> HIR`. As menções a
NRA que restam referem-se à prova de ownership completa de Zith, não à análise
atual.

## 3. Código de erro incorreto para anotações de ownership

**Prioridade: média**

`docs/pages/zith--/reference.md:222` diz que `unique`, `share`, `belong` e
`mut` em posição de argumento reportam `E2007`/`E4007`. A especificação atual
identifica este diagnóstico como `E4007 InvalidCallOwnership`
(`docs/Zith--.md:784`). `E2007` é `NoMatchingFn`, conforme a tabela de
diagnósticos em `docs/impl-status.md:233`.

**Recomendação:** documentar apenas `E4007` para anotações de ownership
inválidas em chamadas.

**Resolvido (2026-11-24).** `take(unique x)` sobre um parâmetro `lend` reporta
`E4007: invalid ownership annotation on a call argument; use 'lend' or 'view'`.
A `reference.md` passou a citar apenas `E4007`. `E2007` continua documentado
como `NoMatchingFn` nas listas de códigos, onde está correto.

## 4. Guia de sintaxe identifica a linguagem errada

**Prioridade: baixa**

`docs/pages/zith--/guide/syntax.md:11` começa por “Zith source”, embora a
página pertença ao guia de Zith--.

**Recomendação:** alterar a frase para identificar explicitamente o código
como fonte Zith--.

**Resolvido (2026-11-24).** `guide/syntax.md` começa com "Zith-- source uses
braces for blocks...".
