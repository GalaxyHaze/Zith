# Zith--

## Objetivo

Zith-- é o subconjunto da linguagem compilado pelo `main` atual da toolchain, documentado em
[`docs/impl-status.md`](impl-status.md). O objetivo desta divisão é manter o `main` pequeno e
verificável: só entra em `Zith--` o que está implementado e protegido por testes nesta iteração,
independentemente de a spec maior `Zith-spec.md` descrever mais features.

Esta iteração mantém os tipos e features existentes, mas restringe bindings, storage e ownership a um núcleo verificável:

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

Um call de método pode ser qualificado com um trait ou interface satisfeita pelo tipo do receiver: `p.Trait.method()` ou `p.Interface.method()`. A qualificação resolve apenas o método visível naquele trait/interface, evitando a ambiguidade `E2008` quando dois traits conformes expõem o mesmo nome:

````zith
trait A { fn pick(self): i32 { return 1 } }
trait B { fn pick(self): i32 { return 2 } }
struct P { x: i32 }
implement P as A {}
implement P as B {}

fn main(): i32 {
    let p = P{ x: 0 };
    return p.A.pick() + p.B.pick();
}
````

Sem a qualificação, `p.pick()` continua ambíguo quando o nome não é resolvido por um método concreto do owner. É esta a forma suportada; `Trait.method(p)` ainda não é aceite.

`dyn Trait` e `dyn Interface` também são suportados no `main`, com uma superfície pública de
**somente métodos**. O fat pointer carrega o data pointer e a vtable do trait/interface; os slots
da vtable apontam para as implementações concretas (ou defaults do trait, quando aplicável).
Campos de interface são usados para conformance e continuam acessíveis em tipos concretos ou
em bounds genéricos, mas nunca através de um valor `dyn`:

````zith
interface Area {
    fn area(self): i32
}

struct Square { side: i32 }
struct Circle { radius: i32 }

implement Square {
    fn area(self): i32 { self.side * self.side }
}
implement Circle {
    fn area(self): i32 { self.radius }
}

fn total(a: dyn Area): i32 { a.area() }

fn main(): i32 {
    return total(Square { side: 3 }) + total(Circle { radius: 5 });
}
````

Um accesso como `a.x` quando `a: dyn Area` e `Area` declara `x` é rejeitado com `E3001`
(field access on non-struct type). Use `is`/cast para um tipo concreto quando precisar do
field, ou um bound genérico `T: Area` se o campo puder ser lido estaticamente.

Funções livres e argumentos de métodos podem declarar parâmetros `lend`/`view`; nesses casos o call site precisa da anotação correspondente para bindings por valor (ver [Ownership](#ownership)).

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

Strings e caracteres decodificam o conjunto de escapes estilo C (`\n`, `\r`, `\t`, `\0`,
`\\`, `\'`, `\"` e `\xHH`). `\$` é aceite como escape adicional e produz um `$` literal; é
útil quando o texto tem de ser preservado sem o tratamento futuro de interpolação com `$`.
Escapes desconhecidos continuam a reportar `E0001`.

## Tipos

Os tipos atuais são mantidos: primitivos, `struct`, `union`, `enum`, `string`, genéricos, function types e as formas compostas existentes. `ptr`, `array`, `slice` e `optional` são modificadores/compostos já existentes, não uma lista excludente de tipos.

### Visibilidade de campos

Os campos de `struct` são privados por defeito. `pub name: T = default` abre explicitamente o
campo para outros módulos; `mod name: T = default` e `mod(N) name: T = default` usam a regra de
visibilidade de módulo existente, com o mesmo significado de `mod`/`mod(N)`/`mod(..)` usado em
declarações:

````zith
struct Box {
    data: i32,          // privado: só visível no ficheiro/module que declara o struct
    pub open: i32,      // público: visível e construtível de outros módulos
    mod sibling: i32,   // visível no ficheiro do struct e no mesmo caminho de módulo
    mod(2) deep: i32,   // visível até duas subdirectorias abaixo do módulo dono
}
````

Fields privados ou `mod` continuam a ser acessíveis dentro do ficheiro que declara o struct,
incluindo métodos desse tipo e funções livres nesse ficheiro. Em struct literals, qualquer field
que não seja acessível a partir do módulo atual é rejeitado; o mesmo diagnóstico é emitido para
field access por `.` ou `->`. Fields privados não deixam de existir no layout, mas não podem ser
mencionados em literais cross-module. Satisfação estrutural de interfaces compara os fields
visíveis a partir do módulo onde a interface é avaliada e exige também method requirements
compatíveis; campos privados continuam disponíveis para satisfação quando o type e a interface
vivem no mesmo ficheiro.

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

`lend` e `view` são o ownership implementado no Zith--. Um parâmetro `p: lend T` ou `p: view T` tem ABI de ponteiro para `T`; no corpo, `p.x` auto-derefs o ponteiro. `lend` continua mutável e `view` bloqueia escrita com `E4004`. Factas NRA residuais continuam a ser emitidas, e codegen aplica `readonly` para `view` e `nocapture` para ambos.

No call site de funções livres, métodos com argumentos por ponteiro e overloads, um binding `default` que alimenta um parâmetro `lend`/`view` exige a anotação `lend x` ou `view x`:

````zith
fn bump(p: lend P): i32 { p.x = p.x + 1; p.x }
fn read(p: view P): i32 { p.x }

fn main(): i32 {
    var q: P = P { x: 41 };
    bump(lend q);   // OK: mutação visível no chamador
    read(view q);   // OK: leitura read-only
    q.x
}
````

Literais, temporários e resultados de chamada não exigem anotação; um binding já qualificado `lend`/`view` também pode ser passado sem nova anotação. A anotação errada em relação ao parâmetro (`lend` para `view` ou `view` para `lend`) e a ausência da anotação num binding `default` reportam `E4005 OwnershipCoercionRequired`. `unique`, `share`, `belong` e `mut` em posição de argumento de call são rejeitados com `E4007 InvalidCallOwnership`.

A exclusividade é validada por chamada e por root lógico do binding: o mesmo binding não pode ser passado duas vezes como `lend`, nem como `lend` + `view`, nem como `view` + `lend`, dentro da mesma chamada. O conflito reporta um único `E4005`. Caminhos de campo/index distintos são raízes distintas neste slice, e `f(p.field)` é aceite para um parâmetro `lend` como borrow daquele campo.

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

O move de receiver é lógico e conservador: invalida usos no sema sem alterar ABI, storage ou chamadas. A exclusividade de `lend`/`view` no call site está implementada; o borrow-checker completo, lifetimes, `unique`, `share`, `belong` e move real de valores ficam para iterações futuras.

## Divisão do Roadmap

A metáfora de gestão é a seguinte: `Zith--` é o subconjunto que o `main` consegue
compilar hoje; `docs/roadmap.md` e `docs/plans/0.7.0/` mapeiam a próxima iteração.
Cada feature candidata só entra no documento do `Zith--` depois de terminar no
pipeline real, porque o `main` não é um modo opt-in.

### Já implementado e provado no `main`

- Funções, bindings `let`/`var`/`const`, structs, enums, unions, génericos e
  monomorfização antes de HIR.
- `when`/`match`, `for`, `state`/`dock`/`jump`, `->`, slices, arrays, opcionais,
  pointers e o protocolo de iterador `next(self)`.
- Qualificadores `lend`/`view` parseados e tipados; anotações `lend x`/`view x`
  em argumentos de call, exclusividade por call e lowering para ponteiros;
  `unique`/`share`/`belong` são rejeitados; `view` bloqueia escrita; receiver move é lógico.
- `defer expr;` e `defer { ... }` como cleanup reverse-order do bloco lexical;
  `state` sem return type explicito é `void`.
- Traits nominais, interfaces estruturais com fields e method requirements
  declaration-only, `implement T as Trait {}`, conformance, bounds
  `T: A + B` e `dyn Trait`/`dyn Interface` somente-métodos; bounds de
  interface expõem fields e métodos no corpo genérico.
- C interop comum, imports, macros normais/raw, C API/zithc, HIR/cache/LLVM.
- O detalhe verificado está em `impl-status.md`; testes focados existem em
  `tests/test-trait-*.cpp`, `tests/test-interface-*.cpp` e
  `tests/test-generic-constraints.cpp`.

### Features candidatas à próxima iteração

Estas são as features que parecem fazer falta ao conjunto atual e que devem ser
avaliadas juntas, por ordem de afinidade com o núcleo:

| Feature | Razão | Dependência mais provável |
| --- | --- | --- |
| `drop` funcional | limpeza de recursos ao sair do binding/escopo (não só keyword) | NRA/ownership residual + HIR |
| `for (x in range)` literal | `0..n` é sintaxe comum que o `when` já usa | `Range`/iterator |
| `dyn Trait` | dispatch dinâmico nominal já está no `main`; falta superfície completa de spec (`view dyn`, slices dyn, etc.) | spec semantics |
| `requires`/`extends` explícitos | já aparecem na spec de traits | implementação de constraints |

A prioridade recomendada é `drop` a seguir, reutilizando a infraestrutura de
escopo de `defer` já implementada, o que fica mais fácil de provar contra a
NRA. Ver `docs/plans/defer-drop.md` para a proposta completa e
`docs/09-control-flow.md` para a semântica de escopo.

### Fora do núcleo até prova em contrário

Ficam fora do `Zith--` e do roadmap de `defer`/`drop`: superfícies `dyn` ainda
spec-only (`view dyn`, slices dyn, etc.), capabilities activadas, `comptime`
completo, reflexão de mutação de tipos,
ownership full NRA, `fail`/`with`/`catch`/`throw`, `use`/contexts/words, assets
e `::` scope resolution. A spec `Zith-spec.md` pode continuar a descrevê-las
como visão, mas a documentação operacional deve marcá-las como `Spec only` ou
`Parse error` até saírem do roadmap.
