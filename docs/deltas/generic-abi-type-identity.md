# Generic ABI And Stable Runtime Type Identity

Status: draft
Target: docs/Zith--.md#objetivo
Owner: -
Related: docs/plans/zith-minusminus-beta-roadmap.md

## Problem

The current non-generic linkage name carries module and parameter context, but
generic instances currently use only the source name and concrete type
spellings. This permits collisions between methods on different generic
owners and between explicit trait implementations. Cache construction also
recovers generic arguments from emitted names instead of structured
instantiation metadata. Runtime opaque tags already use a project-local
registry that persists between sessions, but the initial sequential
assignment has not been proved independent of module compilation order, and
survival of disposable artifact cleanup has not been verified. These gaps
prevent a verified stable-ABI and stable-runtime-identity claim for the public
beta.

## Change

### docs/Zith--.md#objetivo

- change: Zith-- está em beta: a superfície atual está quase completa e o seu comportamento é considerado estável. O trabalho previsto concentra-se em extensões aditivas da linguagem e da standard library, sem expectativa de alterar o comportamento existente. -> Zith-- está em beta: a superfície atual está quase completa e o seu comportamento é considerado estável. A ABI dos módulos e a estabilidade dos runtime opaque tags ainda não são garantidas. A publicação da beta pública fica condicionada à verificação dos contratos de ABI e identidade runtime definidos nesta especificação. O trabalho previsto pode incluir as alterações de compilador necessárias para cumprir esses gates, sem implicar uma reescrita geral da arquitetura.
- add: ## ABI de módulos e identidade de tipos
- add: Zith e Zith-- usam a mesma regra lógica para identificar símbolos de funções. A ABI é específica do target e da sua versão. A estabilidade prometida aplica-se apenas a módulos produzidos para targets e versões de ABI compatíveis.
- add: Para funções não genéricas, a emissão preserva, tanto quanto possível, os nomes de ligação atuais. `extern fn`, aliases declarados com `= extern` e `main` mantêm as respetivas exceções de ligação.
- add: Um símbolo de uma função genérica inclui o módulo que define a declaração, o nome da função e o contexto de assinatura necessário para o distinguir. A identidade inclui os argumentos genéricos concretos através dos canonical type ids de 128 bits, nunca através dos runtime opaque tags `u32`. A representação distingue aridades e ordena primeiro os argumentos genéricos do owner e depois os argumentos da função.
- add: Num método de um owner genérico, a instanciação concreta do owner aparece uma só vez. Os parâmetros usam o padrão de tipos declarado pelo template, não os tipos expandidos da instanciação. O parâmetro implícito `self` e o tipo de retorno não fazem parte da identidade do símbolo.
- add: Num método de uma implementação explícita de trait, a identidade inclui o módulo que define o trait e o seu nome, além do owner e da função.
- add: O cache conserva a identidade da instanciação como metadados estruturados. A hidratação não extrai argumentos genéricos do nome do símbolo.
- add: As grafias `<>` e `@hi:lo` ficam provisórias para representar a instanciação e as duas words do canonical type id. A codificação textual final tem de ser validada em todos os targets da beta antes de a ABI ser declarada estável.
- add: Uma alteração incompatível a nomes públicos, convenções de chamada ou layouts públicos exige uma nova versão de ABI do módulo ou artefacto. A ABI não promete conservar cópias dos símbolos antigos.
- add: A estabilidade dos nomes de símbolos e da ABI não garante compatibilidade dos artefactos de cache entre versões do compilador.

### docs/Zith--.md#opaque-tagged

- change: O runtime opaque tag é um `u32` local ao projeto, atribuído pelo registry persistente a partir do canonical type id e preservado por imports e cache. -> O runtime opaque tag é um `u32` local ao projeto, associado a um canonical type id. Depois de atribuído, o tag desse canonical type id não muda enquanto o estado durável de identidades do projeto for preservado. A mesma identidade produz o mesmo tag em builds frias e quentes, independentemente da ordem de compilação. Adicionar ou remover módulos não renumera tags já atribuídos, incluindo quando um tipo é posteriormente reintroduzido. O estado de identidades não é descartado ao limpar os artefactos de cache. O formato e a localização desse estado são decisões de implementação separadas.

### docs/impl-status.md#functions-bindings

- add: | Generic ABI and runtime type identity | **Release blocker** | Current generic linkage names can collide because they omit owner/module context. Cache instantiation recovery still parses generated names. The project-local opaque-tag registry persists between sessions, but order-independent initial assignment and survival of disposable artifact cleanup are not verified. Do not claim a stable ABI or stable runtime opaque tags until the beta gate passes. |

## Rationale

The symbol identity uses canonical type identities so it remains independent
of the allocation order of project-local runtime tags. The durable identity
state is a stability requirement, not a commitment to a separate lockfile or
to any particular file format. This keeps the design open until the delta is
reviewed and the implementation trade-offs are recorded. The beta roadmap
requires reproducible evidence before these properties can be used as a public
selling point.

The current ABI text spelling and the durable registry's physical location
remain implementation decisions. They must be resolved and validated for the
supported target matrix before the beta publication gate can pass.
