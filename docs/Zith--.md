# Zith--

## Objetivo

Zith-- é uma linguagem de sistemas estaticamente tipada, irmã de Zith. Esta especificação define
o contrato próprio de Zith--. Ela não é um subconjunto de Zith nem uma etapa de desenvolvimento
que evolui para Zith. O compilador `main` implementa Zith--; o estado das features está em
[`docs/impl-status.md`](impl-status.md).

Zith-- está em beta: a superfície atual está quase completa e o seu comportamento é considerado
estável. O trabalho previsto concentra-se em extensões aditivas da linguagem e da standard
library, sem expectativa de alterar o comportamento existente.

A versão atual mantém os tipos e features existentes, com bindings, storage e ownership definidos
por um núcleo verificável:

- `let` para bindings locais imutáveis não-rebindáveis.
- `var` para bindings locais mutáveis e rebindáveis.
- `const` para globals reais, consts locais e campos struct, de storage estático no topo quando aplicável.
- `lend`/`view` continuam como ownership residual; `unique`, `share` e `belong` ficam fora.
- `?T` continua activo como o único tipo de valor ausente/nulável.
- `T!`/result failable fica fora; erros continuam no escopo da espec full Zith.
- Macros normais e `raw macro` continuam activas e são exclusivas do Zith--; `tag` (antigo `tag macro`) fica como item full Zith.
- Atributos `#[...]` incluem `discardable` em funções, `volatile` em variáveis/bindings,
  `test` em funções livres e `alignas(N)` em structs ou bindings suportados.
- `noalias` aplica-se a funções com retorno por ponteiro; `likely` e `unlikely` são reconhecidos
  sem promessa de optimização.

As restrições devem ser aplicadas pela própria toolchain, não apenas documentadas.

## Bindings

A palavra-chave de um binding é preservada pelo frontend e tem estas semânticas:

| Palavra | Escopo | Mutável | Rebindável | Inicializador | Storage |
| --- | --- | --- | --- | --- | --- |
| `let` | Local | Não | Não | Obrigatório para tipo não-trivial | Local |
| `var` | Local | Sim | Sim | Obrigatório para tipo não-trivial | Local |
| `const` | Global ou local | Não | Não | Sempre obrigatório | Estático quando global |

```zith
fn main(): i32 {
    let a: i32 = 1;
    var b: i32 = 2;
    const LOCAL: i32 = 4;
    b = 3;
    return a + b + LOCAL;
}
```

Expressões constantes para `const` são literais numéricos, `bool`, `char` e `null`, agregados desses literais, struct literals com campos `const`, e referências a `const` já declarados. Chamadas de função não contam como expressões constantes.

`let x; x = e;` continua aceite uma vez para inferir o tipo do binding; atribuições seguintes são rejeitadas. `let x: T; x = e;` também aceite a primeira escrita para inicializar o valor, sem alterar o tipo anotado.

A imutabilidade de um binding propaga-se a qualquer caminho de escrita num valor composto: `let p; p.x = 1`, `let p; p->x = 1`, `p.inner.x = 1` e `p[0].x = 1` são rejeitados quando `p` é `let` ou `const` (local ou global). Para `var`, continua permitido escrever campos e elementos aninhados. `view` mantém o bloqueio `WriteThroughView`; `lend` continua mutável. A mensagem para raiz `let`/`const` é `Zith--: cannot write through immutable binding 'p'`.

## Parâmetros e Self

Parâmetros de função são imutáveis por defeito, tal como `let`: `p.x = 1` numa função `fn set(p: P)` é rejeitado. `var p: T` torna o parâmetro localmente mutável, permitindo `p.x = 1`; `let p: T` continua opcional e mantém o default. A atribuição direta ao nome do parâmetro segue a mesma regra: sem `var` é rejeitada, com `var` é permitida na medida em que a assinatura existente continue por valor.

Parâmetros de função podem declarar um default por posição com `= expr` depois do tipo. O default é usado quando o call site omite esse argumento e os argumentos seguintes (dentro do limite de parâmetros fixos) também têm default. Um parâmetro sem default não pode seguir um parâmetro com default, e o tipo da expressão default é verificado contra o tipo do parâmetro. A sintaxe é por posição nesta iteração; não há named arguments nem disambiguation de overloads por defaults.

```zith
fn add(left: i32, right: i32 = 5): i32 {
    return left + right;
}

fn main(): i32 {
    return add(7) + add(1, 2); // 12 + 3
}
```

O `=` interno à lista de parâmetros pertence ao default do parâmetro. Um alias `fn f(...): T = extern name` continua a ser parseado depois do retorno e não colide com a sintaxe de defaults.

## Superfície C e Biblioteca

O namespace raiz `c` contém a superfície C/runtime suportada pelo compilador:

```zith
from c/io
from c/stdlib
from c/string
```

Esses módulos declaram operações C comuns usadas pela stdlib, mas não incluem
implicitamente headers do sistema. O backend pode resolvê-las através da libc
nativa, de intrinsics da VM ou de imports do runtime WASM. `import "file.h"`
continua reservado para APIs específicas da plataforma e bibliotecas C
externas que não pertencem à superfície comum.

`std/io/console` fornece `print` e `println` como API idiomática sobre essa
camada. As funções de saída que devolvem um status mas são normalmente usadas
como statements são marcadas `#[discardable]`; a chamada continua a ser
emitida e o ABI não muda.

Methods continuam com `self` implícito. `self.field` é a forma canónica e auto-derefs o receiver; `self->field` continua aceite como legacy. Um `self` simples é read-only: `self.x = 1` é rejeitado. `var self` permite mutação in-place dos campos do receiver, como `self.x += 1`.

O owner de um bloco `implement` pode ser um primitivo, `?T`, `[]T` ou `*char`, além de um
tipo nomeado:

```zith
trait Value {
    fn value(self): i32;
}
implement i32 as Value {
    fn value(self): i32 { return 7; }
}

trait OptionalValue {
    fn get(self): i32 { return 1; }
}
implement ?char as OptionalValue {}

trait SliceLen {
    fn len(self): i32;
}
implement []char as SliceLen {
    fn len(self): i32 { return 3; }
}

fn main(): i32 {
    let a: i32 = 1;
    let o: ?char = 'x';
    var arr: [3]char = ['a', 'b', 'c'];
    let s: []char = raw arr[0..3];
    return a.value() + o.get() + s.len();  // 7 + 1 + 3
}
```

Estes owners participam na conformação nominal: requirements e defaults são verificados,
duplicatas continuam a falhar, e tipos concretos satisfazem bounds genéricos `T: Trait`.
Pointer genérico (`*T`) e fixed-array (`[N]T`) continuam fora desta iteração, com exceção do
owner canónico `*char`. Num `implement *char`, o `self` simples é o próprio valor `*char`
(não `**char`); `self[0]` lê o primeiro carácter e `.method()`/`->method()` passam o valor do
pointer. `?*char` não participa em coerções de slice nem em owners de implement.

Quando um método com `self` simples ou `var self` é chamado, o sema invalida logicamente a ligação do receiver no chamador: leituras subsequentes reportam `E4001 UseAfterMove`, e escrita através do receiver inválido também. Atribuir diretamente ao nome da variável revive a ligação. `view`/`lend`, receivers explícitos por pointer e chamadas de funções livres ainda não marcam o valor no chamador nesta fase.

Os métodos de `implement *char` são a exceção: o receiver é um valor pointer e a chamada
passa esse valor, por isso `p.method()` e `p->method()` não movem o binding local.

## Pointer Object e `&`

Nesta iteração, `*T` é um pointer object não-nullable e `?*T` é o mesmo object nullable.
`p.x`, `p->x` e `*p` continuam a ser accessos válidos sobre pointers. `&x` é um move lógico
do binding: depois do `&x`, o sema mantém uma versão local nova do mesmo binding para o
pointer resultante, mas o nome `x` fica morto até uma atribuição directa o reviver.
Leituras depois de `&x` reportam `E4001 UseAfterMove`; `raw` continua a ser o escape
explicito para leituras unchecked.

```zith
fn main(): i32 {
    var x: i32 = 10;
    let p: *i32 = &x;   // move lógico: x fica morto
    x = 12;             // revive a versão lógica de x
    return *p;          // o pointer continua a apontar para storage local
}
```

Pointers derivados de `&x` ou `@ptrOf(local)` não podem escapar para escopos mais longos nem
ser guardados como dados persistentes. Devolver esses pointers, guardá-los em structs/arrays,
globals ou aggregados `defer` reporta `E4008 PointerEscopesScope`. Uma chamada por valor com um
argumento pointer é um borrow temporário e fica permitida; `raw` sobre index/deref mantém o
comportamento sem ownership object.

Um call de método pode ser qualificado com um trait ou interface satisfeita pelo tipo do receiver: `p.Trait.method()` ou `p.Interface.method()`. A qualificação resolve apenas o método visível naquele trait/interface, evitando a ambiguidade `E2008` quando dois traits conformes expõem o mesmo nome:

```zith
trait A { fn pick(self): i32 { return 1; } }
trait B { fn pick(self): i32 { return 2; } }
struct P { x: i32 }
implement P as A {}
implement P as B {}

fn main(): i32 {
    let p = P{ x: 0 };
    return p.A.pick() + p.B.pick();
}
```

Sem a qualificação, `p.pick()` continua ambíguo quando o nome não é resolvido por um método concreto do owner. É esta a forma suportada; `Trait.method(p)` ainda não é aceite.

`dyn Trait` e `dyn Interface` também são suportados no `main`, com uma superfície pública de
**somente métodos**. O fat pointer carrega o data pointer e a vtable do trait/interface. Os slots
da vtable apontam para as implementações concretas (ou defaults do trait, quando aplicável).
Campos de interface são usados para conformance e continuam acessíveis em tipos concretos ou
em bounds genéricos, mas nunca através de um valor `dyn`:

```zith
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
```

`?dyn Trait` (e `?dyn Interface`) representa a ausência com o mesmo fat pointer de
`dyn Trait`, usando um data pointer nulo como `null`. Como a representação e o layout são
idênticos aos de `dyn Trait`, `@sizeOf(?dyn Trait)` é igual a `@sizeOf(dyn Trait)`. Um valor
concreto pode ser guardado diretamente num slot `?dyn Trait` (campo, binding, parâmetro,
retorno ou atribuição) e o payload é erguido para o fat pointer antes do wrap. O dispatch de
métodos sobre um recetor `?dyn Trait` só é permitido depois de uma prova de não-nulo, por
exemplo com `if (c is null) { ... }` seguido do uso no ramo restante:

```zith
trait Counter {
    fn get(self): i32
}

struct Cell { v: i32 }

implement Cell as Counter {
    fn get(self): i32 { self.v }
}

fn read(c: ?dyn Counter): i32 {
    if (c is null) { return 0; }
    return c.get();
}
```

Enums e unions também são tipos nomeados genéricos completos. Podem declarar
métodos inline no próprio corpo, participar em `implement Trait`, e os seus
tipos concretos são monomorphizados como `Status<i32>` ou `Any<i32, f64>`:

```zith
trait Value {
    fn value(self): i32 { return 0; }
}

enum Status<T> {
    Ok = 0,
    Err = 1,
    fn code(self): i32 { return @sizeOf(T) as i32; }
}

union Any<T, U> {
    T,
    U,
    fn width(self): i32 { return @sizeOf(T) as i32; }
}

implement Status<T> as Value {
    fn value(self): i32 { return @sizeOf(T) as i32; }
}

implement Any<T, U> as Value {
    fn value(self): i32 { return @sizeOf(T) as i32; }
}

fn report(v: dyn Value): i32 {
    return v.value();
}

fn main(): i32 {
    let status: Status<i32> = Status.Ok;
    var any: Any<i32, f64> = Any<i32, f64>{ 42 };
    let n: i32 = raw any as i32;
    return status.code() + any.width() + report(status) + report(any) - 3 * (@sizeOf(i32) as i32) + n - 42;
}
```

Os discriminantes de variantes continuam a ser constantes inteiras: um template
genérico não pode usar o tipo genérico no discriminante. As variantes são valores
constantes do tipo concreto depois de instanciado (`Status.Ok`), e unions mantêm
o comportamento actual de tagged/raw, incluindo `raw self as T` em métodos
inline.

## Variadic Slices

O último parâmetro de uma função ou método pode ser um variadic slice `[...]T`. Em
`Zith--`, a chamada recolhe todos os argumentos finais homogéneos a partir desse ponto
para um slice temporário. Não há varargs de C com tail heterogéneo:

```zith
fn sum(rest: [...]i32): i32 {
    var total: i32 = 0;
    total = total + raw rest[0];
    total = total + raw rest[1];
    total = total + raw rest[2];
    return total;
}

fn main(): i32 {
    return sum(1, 2, 3); // 6
}
```

O `[...]T` só é permitido na última posição. Os argumentos antes dele são parâmetros
fixos normais. Uma chamada com menos argumentos do que esses parâmetros reporta erro.
Uma chamada pode também passar como último argumento um `[]T` ou `[N]T` já existente:
nesse caso o valor não é recolhido elemento a elemento e o array coerce para o slice
parâmetro. O tail vazio é aceite.

```zith
fn sum(rest: [...]i32): i32 { 0 }

fn main(): i32 {
    sum();                          // tail vazio
    let values: []i32 = [1, 2, 3];
    sum(values);                    // slice explícito
    sum(1, 2, 3);                   // auto-collect homogéneo
}
```

Variadic slices funcionam em funções livres, métodos com `self`, métodos de `dyn
Trait`/`dyn Interface` e funções genéricas. Num parâmetro genérico, `[...]T` infere `T`
a partir do primeiro elemento do tail, ou do tipo do slice/array explícito.
Overloads com arity fixa continuam a preferir a assinatura exata sobre o variadic slice.

Dentro de um pipeline, `..` pode aparecer como argumento do tail: `x |>
f(..)` passa `x` como elemento auto-coletado do slice. `do` também aceita
`..` como argumento opcional, mas pode executar efeitos sem o usar.

## Inferência genérica com `?T`

Um parâmetro que declara um optional com um tipo genérico participa nas coerções opcionais
normais durante a inferência. O compilador tenta primeiro o matching estrutural exacto. Se o
parâmetro é `?T` e o argumento é um valor não-optional, a inferência trata o argumento como
se já estivesse envolvido no optional e liga `T` ao tipo do argumento. A mesma regra aplica-se
a camadas mais fundas, desde que o inner seja generic:

```zith
fn wrap<T>(x: ?T): ?T { return x; }
fn nested<T>(x: ??T): ?T { return x?; }

fn main(): i32 {
    let a: ?i32 = wrap(3);       // T = i32
    let b: ?f64 = wrap<f64>(2.5); // tipo explícito
    let c: ?i32 = nested(5);      // T = i32
    let maybe: ?i32 = 7;
    let d: ?i32 = nested(maybe);  // T = i32, coerção adicional envolvida
    return 0;
}
```

A inferência só usa esta coerção para opcionais. Outras conversões implícitas não propagam
ligacões genéricas nesta iteração.

Um accesso como `a.x` quando `a: dyn Area` e `Area` declara `x` é rejeitado com `E3001`
(field access on non-struct type). Use `is`/cast para um tipo concreto quando precisar do
field, ou um bound genérico `T: Area` se o campo puder ser lido estaticamente.

Funções livres e argumentos de métodos podem declarar parâmetros `lend`/`view`; nesses casos o call site precisa da anotação correspondente para bindings por valor (ver [Ownership](#ownership)).

## Globals

Um global Zith com storage próprio continua a ser declarado com `const`:

```zith
const GLOBAL: i32 = 3;

fn main(): i32 {
    return GLOBAL;
}
```

O HIR emite um nó de global const e o codegen produz um `llvm::GlobalVariable` com linkage `internal`, tipo const e armazenamentos escritos desativados. Referências por nome produzem um load desse global.

Uma declaração `extern` referencia storage definido fora do módulo. `extern var` permite ler e
escrever o objeto. `extern const` permite apenas lê-lo e continua a ser um valor de runtime, não
uma constante de compilação. Ambas as formas exigem um tipo e não aceitam inicializador de valor.
Use `= extern CSymbol` quando o nome do binding Zith diferir do símbolo do linker:

```zith
extern var counter: i32;
extern const rate: f64;
extern var local_name: i32 = extern c_counter;
```

Estas declarações só podem aparecer ao nível do módulo. O linker fornece o objeto externo. O
backend LLVM emite a declaração externa quando o programa referencia o binding. A VM v2 ainda não
suporta executar referências a globals externos e reporta um diagnóstico específico durante o
lowering.

## Const Fields

Campos `const` usam `const name: T = value`, exigem inicializador e só podem aparecer em structs dentro do Zith--:

```zith
struct Point {
    const X_OFFSET: i32 = 2,
    x: i32,
    y: i32,
}

fn main(): i32 {
    var p: Point = Point { x: 1, y: 2 };
    return p.X_OFFSET;
}
```

Campos regulares continuam permitidos, com ou sem default. Atribuição a um campo `const` é rejeitada.

A mesma regra aplica-se a campos `const` dentro de union e a valores de union armazenados por casts/construção: a imutabilidade da raiz não permite alterar o storage interior através de caminhos Root Field/Arrow/Index.

## Enums

Discriminantes não precisam ser literais. Uma variante pode usar uma expressão constante inteira avaliada no módulo:

```zith
const BASE: i32 = 8;

enum Flag {
    ONE = 1,
    SHIFT = 1 << 4,
    OR = 1 |. 4,
    NEG = -1,
    PREV = SHIFT + 1,
    FROM_GLOBAL = BASE,
}
```

São aceites literais inteiros (incluindo `0x`, `0b` e `0c`), `-`/`~` unários, aritmética, operadores bitwise `&.`/`|.`/`^.`, shifts `<<`/`>>`, comparações que resultem em inteiro, variantes anteriores do mesmo enum e `const` globais/locais de tipo inteiro já declarados. Chamadas, casts, literais float/string/bool, `null`, opaques e outros agregados não são aceites. O resultado é avaliado como `int64_t` e, após a avaliação, verificado contra o tipo subjacente do enum; overflow e valores negativos em `uN` são rejeitados.

Strings e caracteres decodificam o conjunto de escapes estilo C (`\n`, `\r`, `\t`, `\\`,
`\'`, `\"`, `\xHH` e octal `\NNN` com um a três dígitos, onde `\0` é a forma de um dígito
para NUL e `\033` é o byte ESC). `\#` é aceite como escape adicional e produz um `#` literal;
é útil quando o texto tem de ser preservado sem o tratamento futuro de interpolação com `#`.
Um literal de carácter tem de decodificar exatamente um carácter. Escapes desconhecidos
reportam `E0003` e são detetados durante a verificação de tipos, tal como no `build`.

## Controlo de Fluxo

O `Zith--` usa condições parentizadas e palavras inglesas para os operadores
booleanos. A negação é `not`; `not (cond)` é a forma idiomática, embora
`not cond` continue a ser aceite. O `!` fica reservado a uma futura forma
postfix e não é um operador unário prefixo nesta iteração.

`return;` termina uma função `void`. Quando `return` transporta uma expressão
o ponto e vírgula é obrigatório: `return expr;`. O parser emite `E1002`
ExpectedSemicolon quando a expressão não é terminada por `;`.

`if` aceita um `else` opcional. A condição encadeada escreve-se na forma
preferida `else (cond) { ... }`; a forma legada `else if (cond) { ... }`
continua aceite, mas emite `W1008` DeprecatedSyntax com a sugestão de
substituição.

`while` continua a compilar, mas emite `W1008` DeprecatedSyntax. Prefira
`for (cond) { ... }`.

`match` é um synonym de `when`; aceita a mesma sintaxe e baixa da mesma forma.

As armas de `when` escrevem-se na forma canónica sem seta: a primeira ilha
parentizada é sempre um pattern contra o subject e o token seguinte é o corpo.
Casos separam-se por vírgula obrigatória (omitida no último) e `(_)` continua a
ser obrigatoriamente o último caso e a única ilha desse caso.

```zith
when (status) {
    (Status.Ok or Status.Error) 10,
    (Status.Queued) and (retries < 5) 20,
    (_) 0
}
```

Na primeira ilha, operandos não-booleanos desugeram para igualdade com o
subject, por isso `(Status.Ok or Status.Error)` significa
`status == Status.Ok or status == Status.Error`. Ilhas seguintes entre
`and`/`or`/`xor` são condições booleanas normais; `(0) and (retries < 5)`
combina o pattern `0` com o guard `retries < 5`. A forma legada
`(cond) ~> body` continua a compilar e baixa da mesma forma, mas emite `W1008`
DeprecatedSyntax.

Os operadores booleanos `or`, `and` e `xor` têm precedência fixa, respetivamente
mais baixa que a comparação: `a or b and c` parsa como `a or (b and c)`, e
`a and b or c` como `(a and b) or c`. `and`/`or` exigem operandos `bool`;
`xor` aceita `bool` ou dois inteiros do mesmo tipo.

### `in` e ranges

`in` é um operador binário normal que devolve `bool`. O RHS resolve o
protocolo `Contains` duck-typed `contains(self, value): bool`; qualquer tipo
com esse método pode ser testado com `value in rhs`. Um literal de range é
um RHS válido sem método adicional e baixa diretamente para as comparações
dos bounds.

Um range literal guarda os bounds brutos e a condição de cada bound:

| Sintaxe | Semântica | AST |
| --- | --- | --- |
| `lo..hi` | `[lo, hi]` | `closed, closed` |
| `lo>..hi` | `(lo, hi]` | `openAtLo` |
| `lo..<hi` | `[lo, hi)` | `openAtHi` |
| `lo>..<hi` | `(lo, hi)` | `openAtLo`, `openAtHi` |

```zith
fn main(x: f64): bool {
    if (x in 0.0>..<1.0) { return true; }
    return x in 2.0..<4.0;
}
```

Ranges float são válidos apenas em `in`/`Contains`; iterar um range float
com `for` é rejeitado porque a iteração de range literal só é definida para
inteiros, com passo implícito `1`.

```zith
fn main(): i32 {
    var total: i32 = 0;
    for (i in 0>..<10) { total = total + i; }
    return total;
}
```

`Iterator` é o protocolo `next(self): ?T` usado por `for (x in iterable)`
sobre valores definidos pelo utilizador e continua separado de
`Contains`.

Em posição de condição, `not expr` sem parêntesis extra é aceite em `if`, `while`, nas três cláusulas do `for (init), (cond), (step)` e em `for (cond)`. A forma canónica dos exemplos e testes é `if not (cond)`; `not cond` e `not (cond)` continuam aceites por compatibilidade. O parser fecha a cláusula depois do operando, por isso também não deixa o parêntesis de fecho pendurado: `for (var i = 0), (not done), (i += 1) { ... }` e `for (var i = 0), (not (done)), ...` são equivalentes.

Uma condição opcional é implícita: qualquer expressão de tipo `?T` é verdadeira quando
tem payload e falsa quando é `null`. Serve para condição (`if`, `while`, `for` e
cláusulas de `for`) e para armas booleanas de `when`. `optional` continua a ser o
modificador de tipo e a forma `optional expr` em posição de condição foi removida.
O teste não faz narrowing do payload; para estreitar continua a usar-se `is null`.

```zith
fn main(x: ?i32): i32 {
    if (x) { return 1; }          // payload presente: teste implícito != null
    return 0;                     // x é null
}
```

Extração explícita de payload usa `must x` (termina com `R10003` em `null`) ou
`raw x` (sem verificação runtime, explícito e arriscado).

Os loops aceitam labels na forma `outer: for ...` e `outer: while ...` para
permitir sair ou continuar a partir de loops aninhados:

Os exemplos de `for` com três cláusulas escrevem-se na forma canónica
`for (init), (cond), (step)`. A forma flat `for (init, cond, step)` também é
aceite e baixa da mesma forma.

`dock State(args)` é uma expressão: chama um state e devolve o valor produzido
pelo `return` no state final. `jump Next(args);` termina o state atual e faz
tail-call direta ao próximo state; a transição valida aridade e tipos contra o
alvo.

```zith
fn scan(xs: []i32): i32 {
    var total: i32 = 0;
    outer: for (var i: i32 = 0), (i < 10), (i = i + 1) {
        for (var j: i32 = 0), (j < 10), (j = j + 1) {
            if (j == 3) { continue outer; }
            if (i == 5) { break outer; }
            total = total + must xs[i * 10 + j];
        }
    }
    return total;
}
```

`break;`/`continue;` sem label referem-se ao loop mais interno. `break outer;`
e `continue outer;` escolhem o loop com esse label ativo; labels desconhecidas
e labels duplicadas entre loops ativos são rejeitadas. O cleanup emitido num
exit labelado cobre todos os blocos entre o ponto atual e o alvo.

`defer expr;` e `defer { ... }` registam cleanup no bloco lexical mais próximo,
em reverse order de registo, e correm no fallthrough e em `return`, `break`,
`continue` e `jump`. Um `defer` pode capturar bindings declarados mais tarde no
mesmo bloco. O sema tipa o corpo adiado depois de conhecer os bindings diretos
do bloco e o HIR emite o cleanup depois de criar os slots. Se um exit antes da
inicialização do binding capturado puder fazer o cleanup correr sem esse valor,
o compilador rejeita com `defer may run before captured binding '<name>' is
initialized`.

## Tipos

Os tipos atuais são mantidos: primitivos, `struct`, `union`, `enum`, genéricos, function types e as formas compostas existentes. `ptr`, `array`, `slice` e `optional` são modificadores/compostos já existentes, não uma lista excludente de tipos.

### Component

`component` define um tipo POD copyable por defeito. Campos são primitivos, arrays/slices desses tipos, ou outros components. Não participa em traits, não permite auto-referência e o layout é C-compatível:

```zith
component Vec2 {
    [x, y]: f32,
    fn sum(self): f32 { self.x + self.y }
}
```

### `type` wrapper e `alias`

`type Name = T` cria um wrapper nominal distinto de `T`. A construção e extração usam casts explícitos:

```zith
type UserId = i32;
alias Answer = i32;

fn makeId(n: i32): UserId { n as UserId }
fn readId(id: UserId): i32 { id as i32 }
```

`alias` continua transparente: `alias Name = T` mantém o tipo subjacente.

### `raw fn`

`raw fn` marca uma função como fronteira não-checada. O corpo pode usar operações `raw` e o compilador não aplica as restrições normais dentro desse limite.

```zith
raw fn readByte(buffer: *u8, index: u64): u8 {
    raw buffer[index]
}
```

### Funções anónimas

Uma função pode ser escrita em posição de expressão, sem nome, com a mesma
cabeça de uma função nomeada menos o identificador: `fn (params): R { body }` ou
`fn (params) { body }`. O valor resultante é uma função de primeira classe do
tipo `fn(params): R`, equivalente a uma referência a uma função nomeada. Não há
lista de captura: o literal não vê bindings locais do escopo onde aparece, apenas
os seus próprios parâmetros e os símbolos de módulo.

```zith
fn apply(f: fn(i32, i32): i32, x: i32): i32 { f(x, x) }

fn main(): i32 {
    let add = fn (a: i32, b: i32): i32 { a + b };
    let square = apply(fn (a: i32, b: i32): i32 { a * b }, 4);
    return add(2, 3) + square;
}
```

O literal baixa como uma função local sintetizada cujo endereço é tomado, por
isso passa e chama-se pela ABI normal de function pointer. Referenciar um
binding do escopo envolvente dentro do corpo é rejeitado como identificador
desconhecido, não capturado.

### Intrinsics de slices, arrays, strings e layout

`@lengthOf(x)` e `@ptrOf(x)` aceitam slices `[]T`, arrays `[N]T`, string literals e
parâmetros `lend`/`view` cujo tipo referenciado seja um slice. Para estes parâmetros, os
intrinsics projectam o descritor emprestado: `@lengthOf` devolve o comprimento e `@ptrOf`
devolve o ponteiro dos elementos, não o endereço do descritor.
`@lengthOf` devolve `u64`; `@ptrOf` devolve `*T` (ou `*char` para strings e slices `[]char`).
Num array local, `@ptrOf` aponta para o slot local e fica sujeito às restrições de escape do
pointer object; em slices devolve o ponteiro ao storage subjacente, sem `raw`.

`@sizeOf(T)` devolve o tamanho de qualquer tipo completo como `u64`. `@alignOf(T)` devolve o
alinhamento natural de qualquer tipo completo como `i32`. `@offsetOf(S, field)` aplica-se
apenas a structs e devolve `i32`.

`@canonicalType(T)` devolve um `u128` estável para o tipo, derivado do módulo, nome e campos
ordenados. Útil para comparar tipos ou usar como chave estável.

### Alinhamento explícito

Use `#[alignas(N)]` para reforçar o alinhamento de uma `struct` ou do storage de um binding
`let`, `var` ou `const` permitido. `N` tem de ser uma expressão constante positiva que resulte
numa potência de dois, e o target tem de suportar esse alinhamento.

```zith
#[alignas(64)] struct TokenBlock { kind: u8 }
#[alignas(64)] const lexerPage: [2]u8 = [0, 0];

fn main(): i32 {
    #[alignas(64)] var scratch: [2]u8 = [0, 0];
    return @alignOf(TokenBlock);
}
```

No tipo, o alinhamento efectivo é o maior entre o alinhamento natural e `N`. O tamanho, os
offsets dos campos, os tipos que contêm a struct e o stride dos seus arrays seguem esse layout.
No binding, apenas o endereço do seu storage é alinhado. O tipo, `@sizeOf`, `@alignOf` e outros
bindings não mudam. Uma cópia usa o alinhamento declarado no binding de destino. Para arrays, a
garantia aplica-se à base do array, não a cada elemento.

O atributo não se aplica a campos nem a parâmetros. Não altera alinhamentos fornecidos por
alocadores para storage dinâmico. A toolchain rejeita valores inválidos ou não suportados e
rejeita a passagem pela ABI C quando não consegue provar o layout exacto do tipo.

```zith
fn main(): i32 {
    var values: [3]i32 = [10, 20, 30];
    let slice: []i32 = raw values[0..3];
    if (@lengthOf(slice) != 3) { return 1; }
    if (raw @ptrOf(slice)[1] != 20) { return 2; }
    if (@lengthOf("zith") != 4) { return 3; }
    if (@sizeOf(i32) != 4) { return 4; }
    0
}
```

String literals têm tipo de origem `*char` e adaptam-se implicitamente a `[]char` quando o
destino é esse slice; não há um builtin `string` novo. Só literals têm esse comprimento em
compile-time: um valor `*char` não-literal não converte implicitamente para `[]char` sem um
comprimento visível. A conversão inversa de `[]char` para `*char` fica disponível e equivale
a `@ptrOf(slice)`, sujeita às regras de escape de `E4008 PointerEscapesScope`. Devolver
`@ptrOf(local)` ou guardá-lo onde a lifetime possa exceder o storage local reporta também
`E4008 PointerEscapesScope`.

### Visibilidade de campos

Os campos de `struct` são privados por defeito. `pub name: T = default` abre explicitamente o
campo para outros módulos; `mod name: T = default` e `mod(N) name: T = default` usam a regra de
visibilidade de módulo existente, com o mesmo significado de `mod`/`mod(N)`/`mod(..)` usado em
declarações:

```zith
struct Box {
    data: i32,          // privado: só visível no ficheiro/module que declara o struct
    pub open: i32,      // público: visível e construtível de outros módulos
    mod sibling: i32,   // visível no ficheiro do struct e no mesmo caminho de módulo
    mod(2) deep: i32,   // visível até duas subdirectorias abaixo do módulo dono
}
```

Fields privados ou `mod` continuam a ser acessíveis dentro do ficheiro que declara o struct,
incluindo métodos desse tipo e funções livres nesse ficheiro. Em struct literals, qualquer field
que não seja acessível a partir do módulo atual é rejeitado. O mesmo diagnóstico é emitido para
field access por `.` ou `->`. Fields privados não deixam de existir no layout, mas não podem ser
mencionados em literais cross-module. Satisfação estrutural de interfaces compara os fields
visíveis a partir do módulo onde a interface é avaliada e exige também method requirements
compatíveis. Campos privados continuam disponíveis para satisfação quando o type e a interface
vivem no mesmo ficheiro.

Um binding local sem inicializador pode ser escrito antes da primeira escrita.
Nesta iteração, qualquer leitura normal antes dessa primeira escrita é rejeitada
com `binding '<name>' is used before it is initialized`. A leitura fuga explícita
é `raw <name>`: preserva o tipo do binding e assume responsabilidade pelo valor
lido, sem alterar layout não inicializado.

```zith
// Aceites, desde que uma escrita ocorra antes de qualquer leitura normal:
let p: *i32;
var s: []i32;
let o: ?i32;
var st: Point;

var n: i32;
let ready: bool;
```

## Ownership

`lend` e `view` são o ownership implementado no Zith--. Um parâmetro `p: lend T` ou `p: view T` recebe o valor por referência: `lend` permite escrita no referente e `view` não. Escrever através de um parâmetro `view` reporta `E4004`.

Em toda a documentação, SRA é a análise de referências/ownership implementada no Zith--
(Small Resource Analysis). O `main` compila o Zith--, que usa exactamente esse subconjunto
congelado. Quando o texto precisar de nomear a análise completa de full Zith, usa `NRA`.

## Opaque tagged

Além do `raw opaque` (`void*`) para interop C, o Zith-- suporta bare `opaque`: uma tagged union
aberta guardada como `{ *void, u32 }`. `T as opaque` erradica um valor endereçável/copyable numa
view para um slot local; `opaque is T` verifica o typeId; `opaque as T` devolve `?T`,
produzindo `null` quando o typeId não corresponde, enquanto `raw opaque as T` reinterpreta sem
verificação. Nesta iteração não há heap copy, vtable ou chamadas dinâmicas. O runtime opaque tag
é um `u32` local ao projeto, atribuído pelo registry persistente a partir do canonical type id e
preservado por imports e cache.

No call site de funções livres, métodos com argumentos por ponteiro e overloads, um binding `default` que alimenta um parâmetro `lend`/`view` exige a anotação `lend x` ou `view x`:

```zith
struct P { x: i32 }

fn bump(p: lend P): i32 { p.x = p.x + 1; p.x }
fn read(p: view P): i32 { p.x }

fn main(): i32 {
    var q = P { x: 41 };
    _ = bump(lend q);   // OK: mutação visível no chamador
    _ = read(view q);   // OK: leitura read-only
    q.x
}
```

Literais, temporários e resultados de chamada não exigem anotação; um binding já qualificado `lend`/`view` também pode ser passado sem nova anotação. A anotação errada em relação ao parâmetro (`lend` para `view` ou `view` para `lend`) e a ausência da anotação num binding `default` reportam `E4005 OwnershipCoercionRequired`. `unique`, `share`, `belong` e `mut` em posição de argumento de call são rejeitados com `E4007 InvalidCallOwnership`.

A exclusividade é validada por chamada e por root lógico do binding: o mesmo binding não pode ser passado duas vezes como `lend`, nem como `lend` + `view`, nem como `view` + `lend`, dentro da mesma chamada. O conflito reporta um único `E4005`. Caminhos de campo/index distintos são raízes distintas neste slice, e `f(p.field)` é aceite para um parâmetro `lend` como borrow daquele campo.

Em unions tagged, `is Tipo` estreita o local testado para o membro dentro do braço `if`/`when` correspondente, sem `as`. Extrair um membro tagged fora desse contexto exige `raw f as Tipo`; unions `raw` mantêm casts livres entre membros.

`unique`, `share` e `belong` são rejeitados com `E2010 UnsupportedSyntax` e mensagem `Zith--: unique/share/belong ownership is not supported; use lend or view`.

O bare `opaque` não é implicitamente compatível com qualquer tipo. `opaque -> T` e `T -> opaque`
exigem o cast explícito: `T as opaque` erradica para `opaque`, `opaque as T` devolve `?T` com
verificação de typeId, e `raw opaque as T` reinterpreta sem verificação. Um binding ou argumento
`let x: u32 = d` quando `d: opaque` reporta
`implicit 'opaque' conversion is not allowed; use 'as' (or 'raw as' for an unchecked extraction)`
e não chega a codegen. Valores bare `opaque` exportados podem ser consumidos por outros módulos.
A cache preserva `canonical_mappings`; se o registry divergir, a hydration reporta `E2010` em vez
de reatribuir tags silenciosamente.

`opaque as raw opaque` é a extração explícita e unchecked do payload como `void*`: devolve
o ponteiro guardado no campo 0 da tagged union, sem verificar o typeId nem fazer load do
payload. Isto é útil para comparar ponteiros reais guardados por valores `opaque`, por exemplo
depois de um teste `value is *char`.

## Macros

Macros normais e `raw macro` continuam activas no Zith-- e são as únicas formas de macro. `tag` é um item da linguagem full Zith (o antigo `tag macro`); o Zith-- rejeita a forma legada com diagnóstico claro:

```zith
// Aceite
macro add(a, b) { a + b }

// Aceite
raw macro dbg(x) { @println(x) }

// Full Zith: `tag` (não `tag macro`)
tag Box(content) { <content/> }

// Rejeitado no Zith--: forma legada de `tag`
tag macro Box(content) { <content/> }
```

## Diagnósticos e warnings

Atributos desconhecidos emitem `W2028 UnknownAttribute`; atributos conhecidos usados
em local ilegal emitem `W2029 AttributeNotApplicable`.

Alguns diagnósticos relevantes não referidos no corpo do documento:

- `E2007` — nenhuma assinatura compatível em overload.
- `E2008` — chamada ambígua.
- `E2024` — interface não satisfeita.
- `E2027` — implementação duplicada.
- `E3005` — dereference de nullable não provado.
- `E3009` — bound de trait não satisfeito.
- `E5001` — IR inválido.
- `E5002` — código inatingível.
- `unsupported external global '<name>' in v2 VM` — a VM v2 não suporta storage
  global externo.

## Imports Platform-Specific

`import foo` continua a ser a única sintaxe de importação de módulos Zith.
Quando o resolver encontra uma importação Zith, tenta primeiro um ficheiro
regular no caminho literal, depois as variantes derivadas do target activo,
e por fim o ficheiro genérico `foo.zith`:

```text
foo.<arch>.<os>.zith
foo.<arch>.zith
foo.<os>.zith
foo.zith
```

Se nenhum ficheiro corresponder e existir um diretório `foo`, o resolver usa
esse diretório como módulo antes de tentar `foo/mod.zith`. Assim, um ficheiro
`foo.zith` tem precedência sobre um diretório homónimo `foo/`.

O target activo usa o triple normalizado e nomes canónicos de `llvm::Triple`
(por exemplo `x86_64`, `aarch64`, `linux`, `darwin`, `windows`); aliases como
`amd64`, `arm64` ou `macos` não são mapeados em v1. Sem LLVM não há variantes
de plataforma. Só o último segmento de um path composto varia, `foo/mod.zith`
continua como diretório de módulo, e paths explícitos, headers C, assets e
literais não são afectados. Se não existir uma variante, `foo.zith`, o
diretório `foo` nem `foo/mod.zith`, a importação reporta
`could not resolve import 'foo'; missing generic module or matching platform
variant`.

`export foo` re-exporta o ficheiro de plataforma resolvido para o target
actual. O artifact, spans e cache apontam para o ficheiro seleccionado.
O cache continua separado por target, porque `CacheKey` já inclui
`targetTriple`.

## Restrições Removidas

| Sintaxe/feature | Motivo | Comportamento |
| --- | --- | --- |
| `global name: T = value` | globals devem usar `const` | `E2010` com sugestão de `const` |
| `mut` como qualificador | bindings usam `var` | `E2010` |
| `const fn` | funções comuns continuam caber no subconjunto | `E2010` |
| `unique`/`share`/`belong` | ownership fora desta iteração | `E2010` |
| `tag` / `tag macro` | `tag` é full Zith; Zith-- aceita apenas macros normais/raw | `E2010` |
| Atribuição a `let`/`const` | imutabilidade | `E2010` |
| Escrita de campo/arrow/index por raiz `let`/`const` | imutabilidade propaga a compósitos | `E2010` |
| Atribuição a campo `const` | storage const | `E2010` |
| `const` sem inicializador | constante precisa de valor | `E2010` |
| Discriminante de enum não constante | variante precisa de valor constante inteiro | `E3001` |
| `!` prefixo | negação usa `not`; `!` fica reservado a postfix | parse error |
| `break label;`/`continue label;` sem label ativo | alvo inexistente ou ambíguo | `E2010` |
| `++` / `--` | sem operadores de incremento/decremento; atualizar com atribuição explícita | `E2010` |

## Fora do Escopo Atual de Zith--

Novos checks de ownership/borrow, heurísticas novas de SRA, `Result` e similares, flag de ativação
ou modo separado. O compilador `main` implementa Zith-- diretamente.

O move de receiver é lógico e conservador: invalida usos no sema sem alterar ABI, storage ou chamadas. A exclusividade de `lend`/`view` no call site está implementada. O borrow-checker completo, lifetimes, `unique`, `share`, `belong` e move real de valores ficam fora do contrato atual de Zith--.

## Roadmap de Zith--

Zith-- e Zith são linguagens irmãs com contratos separados. Este documento especifica Zith--;
`docs/Zith-spec.md` especifica Zith. `docs/roadmap.md` e `docs/plans/0.7.0/` acompanham o
trabalho de implementação para Zith--. `docs/plans/defer-drop.md` preserva a proposta histórica
de `drop`. O ADR-0055 regista design aceite sem prioridade de implementação. Uma feature só passa
a fazer parte do contrato de Zith-- depois de implementada e verificada no pipeline real.

### Já implementado e provado no `main`

- Funções, bindings `let`/`var`/`const`, structs, enums, unions, génericos e
  monomorfização antes de HIR.
- `when`/`match`, `for`, `state`/`dock`/`jump`, pipelines `|>`/`do` com `..`
  explícito, slices, arrays, opcionais, pointers e o protocolo de iterador
  canonico `next(self): ?T` (`??T` para elementos opcionais).
- Funções não-void não podem cair sem valor. Um corpo só termina implicitamente quando um valor
  final tem o tipo certo ou todos os caminhos terminam (`return`, `jump`, `if`/`else` completo,
  `when` com default ou loop infinito sem `break` directo).
- Resultados de call não-void usados como expression statement são considerados
  consumidos-de-outra-forma ou descartados; a única forma explícita de os descartar
  é `_ = expr;` ou um callee `#[discardable]`. Assignment/return/argument/condition/binding usos continuam a
  consumir o valor, e calls `void` continuam legais como statement.
- Uma função `void` não pode terminar com uma expression statement que produza
  valor e não seja assignment, call ou dock call; deve usar `_ = expr;` ou
  declarar um tipo de retorno.
- `state(params): ret` é um tipo de valor para referências a states reais; `let S: state(i32): i32 =
  Machine; dock S(args);` é aceite com as verificações de assinatura do state.
- Qualificadores `lend`/`view` parseados e tipados; anotações `lend x`/`view x`
  em argumentos de call, exclusividade por call e lowering para ponteiros.
  `unique`/`share`/`belong` são rejeitados; `view` bloqueia escrita; receiver move é lógico.
- `defer expr;` e `defer { ... }` como cleanup reverse-order do bloco lexical;
  `state` sem return type explicito é `void`.
- Superfície de teste nativa: `#[test]` em funções livres `void` sem parâmetros,
  os intrínsecos `@assert(cond)`, `@check(cond)` e `@log(msg)`, e o driver
  `main` sintetizado pelo compilador que corre cada `#[test]` e imprime
  `zith-test <nome> <slot>`. `@assert` termina o teste na primeira condição
  falsa. `@check` regista cada condição falsa e continua; fora do contexto
  lexical de um teste, comporta-se como `@assert`. Os IDs de falha e as
  localizações `(teste, id) -> ficheiro:linha:coluna` são gravados na secção
  `Debug` do ZIRL. `zithc test` executa os testes descobertos sob `test_dir`,
  aceita `--filter <substring>` sobre `modulo::funcao` e reporta cada resultado.
  `@log` emite blocos delimitados; o runner oculta logs de testes aprovados,
  excepto com `--verbose`, e mostra os logs de testes falhados.
- `std/test` oferece as macros `assertTrue`, `assertFalse`, `assertEq`,
  `assertNe`, `assertNear`, `assertIn`, `assertNotIn`, `assertOneOf`,
  `assertSome`, `assertNone`, `fail` e os helpers `checkTrue`, `checkFalse`,
  `checkEq`, `checkNe`, `checkNear`, `checkIn`, `checkNotIn`, `checkOneOf`,
  `checkSome`, `checkNone` e `checkFail`. Os helpers `assert*` terminam no
  primeiro resultado falso; os `check*` acumulam falhas e continuam o teste.
  Ambos avaliam cada argumento uma vez e expandem a primitiva no ponto de
  chamada. `assertOneOf` e `checkOneOf` percorrem um array ou slice e passam se
  encontrarem um elemento igual ao valor. `assertNear` e `checkNear` convertem
  os três argumentos para `f64` e comparam a distância absoluta com uma
  tolerância não negativa. `assertEq` e `assertNe` formatam os dois valores com
  `Formatable`, actualmente disponível para `[]char`, `bool`, `f32`, `f64`,
  `u32`, `i32` e `*char`.
- Traits nominais, interfaces estruturais com fields e method requirements
  declaration-only, `implement T as Trait {}`, conformance, bounds
  `T: A + B` e `dyn Trait`/`dyn Interface` somente-métodos; bounds de
  interface expõem fields e métodos no corpo genérico.
- Superfície C/runtime comum em `c/...`, imports, macros normais/raw, C API/zithc,
  HIR/cache/LLVM.
- O detalhe verificado está em `impl-status.md`; testes focados existem em
  `tests/test-trait-*.cpp`, `tests/test-interface-*.cpp` e
  `tests/test-generic-constraints.cpp`.

### Design aceite sem ordem de implementação

As decisões de ownership, pipelines, testes de categoria de tipo e macros
variádicas estão registadas no ADR-0055. Ainda não definem uma ordem de
implementação nem alteram o contrato descrito neste documento. A limpeza
automática por `drop` foi rejeitada para Zith--. A limpeza continua explícita.

### Fora do núcleo até prova em contrário

Ficam fora do `Zith--` e do roadmap atual: as superfícies `dyn` não-método
(`view dyn`, slices `dyn`, etc.), capabilities activadas, `comptime`
completo, reflexão de mutação de tipos,
ownership full NRA, `fail`/`with`/`catch`/`throw`, `use`/contexts/words, assets
e `::` scope resolution. A spec `Zith-spec.md` pode continuar a descrevê-las
como visão, mas a documentação operacional deve marcá-las como `Spec only` ou
`Parse error` até saírem do roadmap.
