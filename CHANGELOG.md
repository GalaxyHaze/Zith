# Changelog


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
