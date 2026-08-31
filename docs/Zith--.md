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
**somente métodos**. O fat pointer carrega o data pointer e a vtable do trait/interface; os slots
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
para um slice temporário; não há varargs de C com tail heterogéneo:

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
fixos normais; uma chamada com menos argumentos do que esses parâmetros reporta erro.
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

## Inferência genérica com `?T`

Um parâmetro que declara um optional com um tipo genérico participa nas coerções opcionais
normais durante a inferência. O compilador tenta primeiro o matching estrutural exacto; se o
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

A inferência só usa esta coerção para opcionais; outras conversões implícitas não propagam
ligacões genéricas nesta iteração.

Um accesso como `a.x` quando `a: dyn Area` e `Area` declara `x` é rejeitado com `E3001`
(field access on non-struct type). Use `is`/cast para um tipo concreto quando precisar do
field, ou um bound genérico `T: Area` se o campo puder ser lido estaticamente.

Funções livres e argumentos de métodos podem declarar parâmetros `lend`/`view`; nesses casos o call site precisa da anotação correspondente para bindings por valor (ver [Ownership](#ownership)).

## Const Global

O único global real/executável é declarado com:

```zith
const GLOBAL: i32 = 3;

fn main(): i32 {
    return GLOBAL;
}
```

O HIR emite um nó de global const e o codegen produz um `llvm::GlobalVariable` com linkage `internal`, tipo const e armazenamentos escritos desativados. Referências por nome produzem um load desse global.

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

Strings e caracteres decodificam o conjunto de escapes estilo C (`\n`, `\r`, `\t`, `\0`,
`\\`, `\'`, `\"` e `\xHH`). `\#` é aceite como escape adicional e produz um `#` literal; é
útil quando o texto tem de ser preservado sem o tratamento futuro de interpolação com `#`.
Escapes desconhecidos continuam a reportar `E0001`.

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

Os operadores booleanos `or`, `and` e `xor` têm precedência fixa, respetivamente
mais baixa que a comparação: `a or b and c` parsa como `a or (b and c)`, e
`a and b or c` como `(a and b) or c`. `and`/`or` exigem operandos `bool`;
`xor` aceita `bool` ou dois inteiros do mesmo tipo.

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

Os exemplos de `for` com três cláusulas escrevem-se sempre na forma
`for (init), (cond), (step)`.

```zith
fn scan(xs: []i32): i32 {
    var total: i32 = 0;
    outer: for (var i: i32 = 0), (i < 10), (i = i + 1) {
        for (var j: i32 = 0), (j < 10), (j = j + 1) {
            if (j == 3) { continue outer; }
            if (i == 5) { break outer; }
            total = total + xs[i * 10 + j];
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
mesmo bloco; o sema tipa o corpo adiado depois de conhecer os bindings diretos
do bloco e o HIR emite o cleanup depois de criar os slots. Se um exit antes da
inicialização do binding capturado puder fazer o cleanup correr sem esse valor,
o compilador rejeita com `defer may run before captured binding '<name>' is
initialized`.

## Tipos

Os tipos atuais são mantidos: primitivos, `struct`, `union`, `enum`, `string`, genéricos, function types e as formas compostas existentes. `ptr`, `array`, `slice` e `optional` são modificadores/compostos já existentes, não uma lista excludente de tipos.

### Intrinsics de slices, arrays e strings

`@lengthOf(x)` e `@ptrOf(x)` aceitam slices `[]T`, arrays `[N]T` e string literals.
`@lengthOf` devolve `u64`; `@ptrOf` devolve `*T` (ou `*char` para strings e slices `[]char`).
Num array local, `@ptrOf` aponta para o slot local e fica sujeito às restrições de escape do
pointer object; em slices devolve o ponteiro ao storage subjacente, sem `raw`.

```zith
fn main(): i32 {
    var values: [3]i32 = [10, 20, 30];
    let slice: []i32 = raw values[0..3];
    if (@lengthOf(slice) != 3) { return 1; }
    if (raw @ptrOf(slice)[1] != 20) { return 2; }
    if (@lengthOf("zith") != 4) { return 3; }
    return 0;
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
que não seja acessível a partir do módulo atual é rejeitado; o mesmo diagnóstico é emitido para
field access por `.` ou `->`. Fields privados não deixam de existir no layout, mas não podem ser
mencionados em literais cross-module. Satisfação estrutural de interfaces compara os fields
visíveis a partir do módulo onde a interface é avaliada e exige também method requirements
compatíveis; campos privados continuam disponíveis para satisfação quando o type e a interface
vivem no mesmo ficheiro.

Para `let`/`var`, um tipo é não-trivial quando não é primitivo escalar (`iN`/`uN`/`fN`, `bool`, `char`, `void`). Tipos não-triviais sem inicializador são rejeitados:

Um binding local sem inicializador pode ser escrito antes da primeira escrita;
nesta iteração, qualquer leitura normal antes dessa primeira escrita é rejeitada
com `binding '<name>' is used before it is initialized`. A leitura fuga explícita
é `raw <name>`: preserva o tipo do binding e assume responsabilidade pelo valor
lido, sem alterar layout não inicializado.

```zith
// Rejeitados:
let p: *i32;
var s: []i32;
let o: ?i32;
var st: Point;

// Aceites em locais:
var n: i32;
let ready: bool;
```

## Ownership

`lend` e `view` são o ownership implementado no Zith--. Um parâmetro `p: lend T` ou `p: view T` tem ABI de ponteiro para `T`; no corpo, `p.x` auto-derefs o ponteiro. `lend` continua mutável e `view` bloqueia escrita com `E4004`. Factas NRA residuais continuam a ser emitidas, e codegen aplica `readonly` para `view` e `nocapture` para ambos.

Em toda a documentação, NRA é a análise completa de referências/ownership do Zith
(Reference Analysis). O `main` compila o Zith--, que implementa uma versão parcial e
simplificada dessa análise; quando o texto precisar de nomear esse subconjunto, usa
`Reference Analysis (simplified)`.

## Opaque tagged

Além do `raw opaque` (`void*`) para interop C, o Zith-- suporta bare `opaque`: uma tagged union
aberta guardada como `{ *void, u32 }`. `T as opaque` erradica um valor endereçável/copyable numa
view para um slot local; `opaque is T` verifica o typeId; `opaque as T` devolve `?T`,
produzindo `null` quando o typeId não corresponde, enquanto `raw opaque as T` reinterpreta sem
verificação. Nesta iteração não há heap copy, vtable ou chamadas dinâmicas, e o typeId é interno
e module-local (atravessar imports/cache de bare `opaque` reporta `E2010`).

No call site de funções livres, métodos com argumentos por ponteiro e overloads, um binding `default` que alimenta um parâmetro `lend`/`view` exige a anotação `lend x` ou `view x`:

```zith
fn bump(p: lend P): i32 { p.x = p.x + 1; p.x }
fn read(p: view P): i32 { p.x }

fn main(): i32 {
    var q: P = P { x: 41 };
    bump(lend q);   // OK: mutação visível no chamador
    read(view q);   // OK: leitura read-only
    q.x
}
```

Literais, temporários e resultados de chamada não exigem anotação; um binding já qualificado `lend`/`view` também pode ser passado sem nova anotação. A anotação errada em relação ao parâmetro (`lend` para `view` ou `view` para `lend`) e a ausência da anotação num binding `default` reportam `E4005 OwnershipCoercionRequired`. `unique`, `share`, `belong` e `mut` em posição de argumento de call são rejeitados com `E4007 InvalidCallOwnership`.

A exclusividade é validada por chamada e por root lógico do binding: o mesmo binding não pode ser passado duas vezes como `lend`, nem como `lend` + `view`, nem como `view` + `lend`, dentro da mesma chamada. O conflito reporta um único `E4005`. Caminhos de campo/index distintos são raízes distintas neste slice, e `f(p.field)` é aceite para um parâmetro `lend` como borrow daquele campo.

Em unions tagged, `is Tipo` estreita o local testado para o membro dentro do braço `if`/`when` correspondente, sem `as`. Extrair um membro tagged fora desse contexto exige `raw f as Tipo`; unions `raw` mantêm casts livres entre membros.

`unique`, `share` e `belong` são rejeitados com `E2010 UnsupportedSyntax` e mensagem `Zith--: unique/share/belong ownership is not supported; use lend or view`.

O bare `opaque` não é implicitamente compatível com qualquer tipo. `opaque -> T` e `T -> opaque` exigem o cast explícito: `T as opaque` erradica para `opaque`, `opaque as T` devolve `?T` com verificação de typeId, e `raw opaque as T` reinterpreta sem verificação. Um binding ou argumento `let x: u32 = d` quando `d: opaque` reporta `implicit 'opaque' conversion is not allowed; use 'as' (or 'raw as' for an unchecked extraction)` e não chega a codegen. Bare `opaque` continua module-local: atravessar imports/cache reporta `E2010`.

`opaque as raw opaque` é a extração explícita e unchecked do payload como `void*`: devolve
o ponteiro guardado no campo 0 da tagged union, sem verificar o typeId nem fazer load do
payload. Isto é útil para comparar ponteiros reais guardados por valores `opaque`, por exemplo
depois de um teste `value is *char`.

## Macros

Macros normais e `raw macro` continuam ativas. Tag macros são rejeitadas com diagnóstico claro:

```zith
// Aceite
macro add(a, b) { a + b }

// Aceite
raw macro dbg(x) { @println(x) }

// Rejeitado no Zith--
tag macro Box(content) { <content/> }
```

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
| `!` prefixo | negação usa `not`; `!` fica reservado a postfix | parse error |
| `break label;`/`continue label;` sem label ativo | alvo inexistente ou ambíguo | `E2010` |

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
  pointers e o protocolo de iterador canonico `next(self): ?T` (`??T` para elementos opcionais).
- Funções não-void não podem cair sem valor; um corpo só termina implicitamente quando um valor
  final tem o tipo certo ou todos os caminhos terminam (`return`, `jump`, `if`/`else` completo,
  `when` com default ou loop infinito sem `break` directo).
- `state(params): ret` é um tipo de valor para referências a states reais; `let S: state(i32): i32 =
  Machine; dock S(args);` é aceite com as verificações de assinatura do state.
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
