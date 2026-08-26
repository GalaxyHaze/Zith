# Zith--

## Objetivo

Zith-- é o subconjunto da linguagem compilado pelo `main` atual da toolchain. Esta iteração mantém os tipos e features existentes, mas restringe bindings, storage e ownership a um núcleo verificável:

- `let` para bindings locais imutáveis não-rebindáveis.
- `var` para bindings locais mutáveis e rebindáveis.
- `const` para globals reais, consts locais e campos struct, de storage estático no topo quando aplicável.
- `lend`/`view` continuam como ownership residual; `unique`, `share` e `belong` ficam fora.
- Macros normais e `raw macro` continuam ativas; tag macros ficam fora.

As restrições devem ser aplicadas pela própria toolchain, não apenas documentadas.

## Bindings

A palavra-chave de um binding é preservada pelo frontend e tem estas semânticas:

| Palavra | Escopo | Mutável | Rebindável | Inicializador | Storage |
| --- | --- | --- | --- | --- | --- |
| `let` | Local | Não | Não | Obrigatório para tipo não-trivial | Local |
| `var` | Local | Sim | Sim | Obrigatório para tipo não-trivial | Local |
| `const` | Global ou local | Não | Não | Sempre obrigatório | Estático quando global |

````zith
fn main(): i32 {
    let a: i32 = 1;
    var b: i32 = 2;
    const LOCAL: i32 = 4;
    b = 3;
    return a + b + LOCAL;
}
````

Expressões constantes para `const` são literais numéricos, `bool`, `char` e `null`, agregados desses literais, struct literals com campos `const`, e referências a `const` já declarados. Chamadas de função não contam como expressões constantes.

`let x; x = e;` continua aceite uma vez para inferir o tipo do binding; atribuições seguintes são rejeitadas. `let x: T; x = e;` também aceite a primeira escrita para inicializar o valor, sem alterar o tipo anotado.

A imutabilidade de um binding propaga-se a qualquer caminho de escrita num valor composto: `let p; p.x = 1`, `let p; p->x = 1`, `p.inner.x = 1` e `p[0].x = 1` são rejeitados quando `p` é `let` ou `const` (local ou global). Para `var`, continua permitido escrever campos e elementos aninhados. `view` mantém o bloqueio `WriteThroughView`; `lend` continua mutável. A mensagem para raiz `let`/`const` é `Zith--: cannot write through immutable binding 'p'`.

## Parâmetros e Self

Parâmetros de função são imutáveis por defeito, tal como `let`: `p.x = 1` numa função `fn set(p: P)` é rejeitado. `var p: T` torna o parâmetro localmente mutável, permitindo `p.x = 1`; `let p: T` continua opcional e mantém o default. A atribuição direta ao nome do parâmetro segue a mesma regra: sem `var` é rejeitada, com `var` é permitida na medida em que a assinatura existente continue por valor.

Methods continuam com `self` implícito. `self.field` é a forma canónica e auto-derefs o receiver; `self->field` continua aceite como legacy. Um `self` simples é read-only: `self.x = 1` é rejeitado. `var self` permite mutação in-place dos campos do receiver, como `self.x += 1`.

Quando um método com `self` simples ou `var self` é chamado, o sema invalida logicamente a ligação do receiver no chamador: leituras subsequentes reportam `E4001 UseAfterMove`, e escrita através do receiver inválido também. Atribuir diretamente ao nome da variável revive a ligação. `view`/`lend`, receivers explícitos por pointer e chamadas de funções livres ainda não marcam o valor no chamador nesta fase.

## Const Global

O único global real/executável é declarado com:

````zith
const GLOBAL: i32 = 3;

fn main(): i32 {
    return GLOBAL;
}
````

O HIR emite um nó de global const e o codegen produz um `llvm::GlobalVariable` com linkage `internal`, tipo const e armazenamentos escritos desativados. Referências por nome produzem um load desse global.

## Const Fields

Campos `const` usam `const name: T = value`, exigem inicializador e só podem aparecer em structs dentro do Zith--:

````zith
struct Point {
    const X_OFFSET: i32 = 2,
    x: i32,
    y: i32,
}

fn main(): i32 {
    var p: Point = Point { x: 1, y: 2 };
    return p.X_OFFSET;
}
````

Campos regulares continuam permitidos, com ou sem default. Atribuição a um campo `const` é rejeitada.

A mesma regra aplica-se a campos `const` dentro de union e a valores de union armazenados por casts/construção: a imutabilidade da raiz não permite alterar o storage interior através de caminhos Root Field/Arrow/Index.

## Enums

Discriminantes não precisam ser literais. Uma variante pode usar uma expressão constante inteira avaliada no módulo:

````zith
const BASE: i32 = 8;

enum Flag {
    ONE = 1,
    SHIFT = 1 << 4,
    OR = 1 |. 4,
    NEG = -1,
    PREV = SHIFT + 1,
    FROM_GLOBAL = BASE,
}
````

São aceites literais inteiros (incluindo `0x`, `0b` e `0c`), `-`/`~` unários, aritmética, operadores bitwise `&.`/`|.`/`^.`, shifts `<<`/`>>`, comparações que resultem em inteiro, variantes anteriores do mesmo enum e `const` globais/locais de tipo inteiro já declarados. Chamadas, casts, literais float/string/bool, `null`, opaques e outros agregados não são aceites. O resultado é avaliado como `int64_t` e, após a avaliação, verificado contra o tipo subjacente do enum; overflow e valores negativos em `uN` são rejeitados.

## Tipos

Os tipos atuais são mantidos: primitivos, `struct`, `union`, `enum`, `string`, genéricos, function types e as formas compostas existentes. `ptr`, `array`, `slice` e `optional` são modificadores/compostos já existentes, não uma lista excludente de tipos.

Para `let`/`var`, um tipo é não-trivial quando não é primitivo escalar (`iN`/`uN`/`fN`, `bool`, `char`, `void`). Tipos não-triviais sem inicializador são rejeitados:

````zith
// Rejeitados:
let p: *i32;
var s: []i32;
let o: ?i32;
var st: Point;

// Aceites em locais:
var n: i32;
let ready: bool;
````

## Ownership

`lend` e `view` continuam parseados, tipados e com o comportamento residual atual. `view` bloqueia escrita e as factas NRA continuam a ser emitidas. Em receiver explícitos, `self: lend Owner` e `self: view Owner` têm ABI e corpo de ponteiro para `Owner`; chamadas com `.` passam o endereço do receiver, por isso mutações via `lend`/`*` são visíveis no chamador.

Em unions tagged, `is Tipo` estreita o local testado para o membro dentro do braço `if`/`when` correspondente, sem `as`. Extrair um membro tagged fora desse contexto exige `raw f as Tipo`; unions `raw` mantêm casts livres entre membros.

`unique`, `share` e `belong` são rejeitados com `E2010 UnsupportedSyntax` e mensagem `Zith--: unique/share/belong ownership is not supported; use lend or view`.

## Macros

Macros normais e `raw macro` continuam ativas. Tag macros são rejeitadas com diagnóstico claro:

````zith
// Aceite
macro add(a, b) { a + b }

// Aceite
raw macro dbg(x) { @println(x) }

// Rejeitado
tag macro Box(content) { <content/> }
````

## Restrições Removidas

| Sintaxe/feature | Motivo | Comportamento |
| --- | --- | --- |
| `global name: T = value` | globals devem usar `const` | `E2010` com sugestão de `const` |
| `mut` como qualificador | bindings usam `var` | `E2010` |
| `const fn` | funções comuns continuam caber no subconjunto | `E2010` |
| `unique`/`share`/`belong` | ownership fora desta iteração | `E2010` |
| Tag macros | só permanecem macros normais/raw | `E2010` |
| Atribuição a `let`/`const` | imutabilidade | `E2010` |
| Escrita de campo/arrow/index por raiz `let`/`const` | imutabilidade propaga a compósitos | `E2010` |
| Atribuição a campo `const` | storage const | `E2010` |
| `const` sem inicializador | constante precisa de valor | `E2010` |
| `let`/`var` não-triviais sem inicializador | evita valor não inicializado | `E2010` |
| Discriminante de enum não constante | variante precisa de valor constante inteiro | `E3001` |

## Não É Desta Iteração

Novos checks de ownership/borrow, heurísticas novas de NRA, `Result` e similares, flag de ativação ou modo separado. O `main` é o Zith--.

O move de receiver é lógico e conservador: invalida usos no sema sem alterar ABI, storage ou chamadas; exclusividade de `lend`/`view` e move real de valores ficam para iterações futuras.
