# Zith-- Implementation Notes

## Fonte de Verdade

Este arquivo documenta como a toolchain implementa e verifica o subconjunto Zith-- definido em `docs/Zith--.md`. A linguagem compilada pelo `main` é sempre Zith--; não há frontend separado, flag de ativação ou modo opt-in.

## Pipeline de Emissão

O pipeline distingue as representações por estágio:

```text
Source -> Lex -> Parse/CST -> AST -> Sema -> HIR -> VIR -> Codegen/Cache
```

`--emit-cst` imprime a Concrete Syntax Tree depois do parsing, com nós, spans e tokens.
`--emit-ast` imprime a árvore frontend depois do lowering inicial. `--emit-hir` imprime o
High-level IR depois do lowering semântico. `--emit-vir` baixa HIR para VM v2 através de
`vm::lowerModule()` e imprime o Typed VM v2 IR. VIR significa VM v2 IR e não LLVM IR.

As emissões são cumulativas. `--emit-all` inclui CST e VIR além de tokens, AST, HIR, LLVM IR e
assembly. No WASM, todas as emissões textuais usam o buffer de saída da sessão, sem ficheiros
temporários. O blob HIR da ABI WASM continua separado e não contém CST nem VIR.

## Frontend

O AST modela bindings com `BindingKind`:

- `BindingKind::Let` para `let`.
- `BindingKind::Var` para `var`.
- `BindingKind::Const` para `const`.

`Declaration.bindingKind` é definido para `DeclKind::Variable`. `Binding.bindingKind` é definido para `StmtKind::Binding`. O parser:

- Aplica `Parameter.bindingKind` com default `Let` e lê `var p: T` ou `let p: T` antes do nome do parâmetro; `var self`/`let self` usam o mesmo campo no receiver.
- Depois do tipo de um parâmetro, reconhece `= expr` e guarda a expressão em `Parameter.defaultValue`; o `= extern name` de um alias de função só é tratado depois do tipo de retorno.
- Aplica o default `BindingKind::Let` e corrige com a palavra-chave real.
- Baixa `[a, b]` para uma binding temporária e bindings locais posicionais. Uma
  anotação escalar gera inicializadores que referenciam a mesma temporária;
  uma anotação pack ou um inicializador pack literal mantém a extração por
  índice. Aliases transparentes locais, não genéricos e não ambíguos são
  seguidos para distinguir tipos pack de tipos escalares.
- Rejeita `global` antes de baixar a declaração, com recovery para `DeclKind::Variable`.
- Parseia `extern var name: T` e `extern const name: T` como declarations externas ao nível do módulo. `= extern CSymbol` guarda um alias de linker, não um inicializador.
- Rejeita `const fn` em `functionKindPrefix`.
- Rejeita tag macros em `lowerMacroDeclaration`.
- Rejeita `const name: T` (global ou local) sem inicializador, exceto declarações externas `extern const`.
- Rejeita `const name: T` sem valor em struct fields.
- Parseia `const X: T = value` em `parseStructField` e marca `Parameter.isConstField`.
- Parseia `pub name: T`, `mod name: T`, `mod(N) name: T` e `mod(..) name: T` antes do nome do
  field e regista `Parameter.visibility`/`Parameter.modDepth`; cada field sem prefixo fica
  `Visibility::Private`. A forma aplica-se também a fields agrupados `[x, y]: T`.

Qualificadores `mut`, `unique`, `share` e `belong` são parseados para recovery/formatter, mas emitem `E2010 UnsupportedSyntax`.

`fn` em posição de expressão (`parseFunctionLiteral()` em `frontend-expr-primary.cpp`) é reconhecido como átomo primário antes do resto de `parsePrimary()`. O literal baixa como uma declaração `DeclKind::Function` sintetizada com `Declaration::isAnonymousFunction = true` e um nome `$anon<id>` que nenhuma expressão fonte consegue escrever. O parser troca o escopo corrente por `root_scope_` enquanto baixa a cabeça e o corpo, por isso o corpo não vê bindings locais do escopo envolvente: uma referência a esses bindings falha como `E2001 unknown identifier` em vez de ser capturada. A resolução de símbolos ignora o nome sintético (nunca cria um binding visível para ele) mas continua a registar os parâmetros dentro do corpo. O resultado é um `Expression` com `kind = ExprKind::FunctionLiteral` e `declaration` a apontar para a função sintetizada.

O parser cria `ExprKind::OwnershipCoerce` apenas em listas de argumentos de call (`parseCallArgument()`), usado nas chamadas `f(...)`, `f<G>(...)`, dock e jump. Só `lend` e `view` são aceites como anotação de argumento; `unique`/`share`/`belong`/`mut` nessa posição reportam `E4007 InvalidCallOwnership`. Fora de um call argument, `lend`/`view` continuam a ser tratados como keywords normais e falham com o diagnóstico normal.

`parseConditionExpression()` é usado em `if`, `while`, nas três cláusulas do `for` e nos grupos parentizados `for (cond)`. A função reconhece `not expr` e constrói `ExprKind::Unary` com texto `not` antes de `parseExpression()`. O token seguinte `)`/`,`/`{` interrompe o sugar, para que o parser não absorva o fecho ou separador da cláusula.

`parseStatements()` mantém `return;` e `return` antes de `}` como formas sem
valor. Quando `return` tem uma expressão, o parser exige `;` depois de
`parseExpression()` e reporta `E1002 ExpectedSemicolon` sem consumir mais
tokens.

`_ = expr;` baixa para `StmtKind::Discard` no parser quando o token inicial é
`_` seguido de `=`; o parser consome o par e exige `;` após a expressão. O sema
continua a inferir a expressão descartada mas não a devolve como valor do bloco.
Em `inferBlock`, uma expression statement cujo AST root é `Call`/`DockCall` com
tipo não-void/não-error reporta `E2026 DiscardedResult`, exceto quando o callee
é `#[discardable]` ou quando a
expressão é o valor final de uma função não-void; uma function void continua a
exigir `StmtKind::Discard`. `StmtKind::Discard` é a única forma explícita de
aceitar esse tipo. Em `checkReturnsAndCalls`, uma função void que termina com
uma expression statement que produz valor e cuja raiz não é `Assign`, `Call` nem
`DockCall` também reporta `E2026 DiscardedResult`. O HIR baixa
`StmtKind::Discard` como a chamada executada, sem a tornar no valor implícito do
bloco.

`parseIf()` aceita um `else` simples, `else if (cond) { body }` e a forma
preferida `else (cond) { body }`. Ambos os caminhos encadeados usam o mesmo
`ExprKind::If` com operandos `[cond, then, else-cond-or-inner-if, else-body]`;
só `else if` emite `W1008 DeprecatedSyntax`. A distinção de spelling no
formatter é feita pelo token inicial do operando `else` no AST.

`parseWhen()` aceita a forma canónica sem seta: depois do subject e das ilhas
parentizadas, o token seguinte é o corpo do caso. Cada caso separa-se por
vírgula obrigatória (omitida no último) e `(_)` continua a ser a única ilha do
último caso. A primeira ilha é construída como `ExprKind::WhenGuard`; as ilhas
seguintes dentro do mesmo par são lidas como expressões booleanas ligadas por
`and`/`or`/`xor` no topo, com os operadores guardados em `whenIslandOps`. A
forma legada `(cond) ~> body` continua aceite, mas o parser emite uma vez
`W1008 DeprecatedSyntax` por braço e baixa o caso como antes.

`parseExpression()` reconhece os operadores keyword `and`, `or` e `xor` no laço
binário com precedências 3, 2 e 4, em paralelo com `FmtVisitor::binaryPrecedence`.
`parseConditionExpression()` delega os operandos para esse laço, por isso
`if (a and not b)` e `if (a or b)` consomem apenas a condição e voltam ao
`)/,/{` esperado pelo chamador.

O lexer agrupa `..` e `...` num único `TokenKind::Dots`, distinguindo-os pelo lexeme. O range literal, o placeholder de pipeline, os imports relativos, a profundidade `mod(..)` e os variadic slices consomem esse token em vez de dois pontos separados. `++` e `--` são lexados e rejeitados com `E2010 UnsupportedSyntax`; a mensagem pede atribuição explícita ao valor atualizado.

## SRA (Small Resource Analysis)

O Zith-- não implementa o NRA completo. O que existe é o SRA, uma análise
deliberadamente pequena e congelada: não vai crescer para a máquina de prova
completa, porque contracts, NRA e safety pertencem ao Zith completo. O modelo
futuro está em `docs/nra-spec.md`.

O SRA cobre exatamente:

- o slice `lend`/`view` de anotações de borrow em chamadas (parâmetros
  `lend T`/`view T`, `E4004`/`E4005`/`E4007`, hints LLVM `readonly` e
  `nocapture`);
- conflitos de borrow locais no sema (`E4002` BorrowConflict, `E4003`
  DoubleBorrow);
- use-after-move determinístico de `&local` e `@ptrOf(local)`, publicado como
  `HirConsumedState::Consumed` em `HirAttrs`;
- factos residuais pré-lowering acumulados em `SraFacts` e consumidos como
  side tables, sem nós de ownership no HIR.

`own`/`share`/`belong`/`mut` como prefixos de tipo ficam rejeitados ou
legacy-only, e `:=` não existe em Zith--.

## Atributos

O frontend parseia grupos `#[name, ...]` antes de visibilidade/declarações
top-level, métodos, e bindings locais. `AttributeKind` é resolvido por
`classifyAttribute`: `discardable` aplica-se a funções e `volatile` a
variáveis/bindings; atributos desconhecidos emitem `W2028 UnknownAttribute` e
atributos conhecidos em local ilegal emitem `W2029 AttributeNotApplicable`.

`discardable` propaga de `Declaration` para resolução e `SymbolData`, e o
lowering regista `HirFnAttrs.discardable`. Sema omite `E2026 DiscardedResult`
quando o callee resolvido tem o atributo, inclusive cross-module.

`volatile` assinala slots como `HirSlotAttrs.volatileSlot`. NRA não acumula
factos de ownership/narrowing para esses locais; codegen emite loads e stores
LLVM volatile para o slot. O cache serializa `discardable` e `volatileSlot` no
Code/attrs sections e a ABI hash inclui `SymbolData.discardable`. O formatter
reemite grupos `#[name, ...]` antes da declaração/binding e preserva a ordem
original.

`alignas(N)` aceita structs comuns e bindings `let`, `var` ou `const` permitidos.
Sema exige uma constante positiva, potência de dois, independente de parâmetros
genéricos, e rejeita alinhamentos que o target não suporta. Em structs, o tipo
guarda o alinhamento explícito e o layout deriva offsets, tamanho e stride de
arrays desse alinhamento. Em bindings, o HIR guarda o alinhamento no slot local
ou no global, sem o propagar ao tipo nem a cópias. O codegen LLVM aplica-o ao
`alloca` ou global correspondente.

O cache serializa o alinhamento do tipo, dos slots e dos globals. Os schemas
que mudaram rejeitam explicitamente artefactos antigos incompatíveis. A VM v2
baixa arrays locais explicitamente alinhados para storage próprio e solicita o
alinhamento ao alocador da memória linear. O HIR interpreter representa locais
com células e objetos abstractos, sem expor endereços nativos. A fronteira C
rejeita structs alinhadas por valor enquanto o compilador não provar o layout
exacto para o target.

`noalias` só é aceite em funções com retorno por ponteiro. `likely` e
`unlikely` são reconhecidos, mas não prometem comportamento específico do
optimizador.

## Superfície C/runtime da stdlib

Os módulos `stdlib/c/*.zith` são bindings C comuns mantidos pela toolchain,
não includes de headers do host. A stdlib usa `from c/io`, `from c/stdlib` e
`from c/string` para manter o mesmo código em LLVM nativo, VM no-LLVM e WASM.
O backend escolhe se cada declaração é resolvida por libc, intrinsic da VM ou
import do runtime.

`import "file.h"` continua no caminho de interop específico: APIs de plataforma,
SDKs e bibliotecas externas. O binder de headers não é requisito para carregar a
superfície comum da stdlib.

## Sema

Uma função sem tipo de retorno explícito infere o resultado pela expressão final do corpo e
pelos `return` com valor. Corpos sem resultado inferem `void`. Os tipos dos retornos têm de
coincidir ou aceitar a coerção já permitida pelo sema. Ciclos de chamadas que envolvam funções
com retorno inferido, diretos ou indiretos, exigem uma anotação explícita e reportam `E3002`.
O sema não infere retornos `union` ou `dyn`, que também exigem anotação.

O `SemaPipeline` infere estas assinaturas antes da análise normal para disponibilizá-las a
chamadores declarados anteriormente. Esse passe provisório suprime diagnósticos semânticos
comuns e descarta os facts de expressão antes da passagem final. Mantém os tipos locais, pois
bindings anotados sem inicializador e parâmetros já têm tipos válidos; o tracking flow-sensitive
de atribuições é isolado para não tratar a primeira atribuição de um `let` como uma segunda.

`checkZithDeclarations` roda dentro de `checkExpressions` e verifica:

- Todo `const` tem inicializador.
- Todo inicializador de `const` é uma expressão constante.
- `let`/`var` com tipo não-trivial têm inicializador.
- Bindings locais `const` têm inicializador.

`isConstantExpression` aceita literais, agregados de literais, struct literals constantes e referências a `const` globais/locais. Outras expressões, incluindo chamadas, são rejeitadas.

`extern const` é uma referência read-only a storage de runtime. O alias `= extern CSymbol` não
fornece um valor nem torna o binding uma expressão constante.

Atribuições são verificadas em `inferAssign`:

- `let` com inicializador é imutável.
- `let` sem inicializador aceita a primeira escrita, que pode inferir ou manter o tipo anotado.
- `const` global/local e `extern const` nunca podem ser atribuídos. `extern var` aceita atribuição.
- `checkConstFieldAssignments` rejeita escrita em campo marcado `isConstField`.
- Para globals importados, apenas a constness do objeto C controla a mutabilidade do binding. A
  constness do pointee de um pointer não torna o próprio objeto read-only.

`targetFieldIsConst` percorre o caminho de uma place expression para detectar campos const, incluindo bases aninhadas e acesso por pointer.

Além do check de ownership existente (`checkAssignableOwnership`), `inferAssign` chama
`checkImmutableRootFieldWrite`. Essa função usa `assignmentRoot` para encontrar a raiz da
place expression e rejeita escritas de campo, arrow ou index cuja raiz seja `let` ou `const`
(local ou global) com `Zith--: cannot write through immutable binding '<name>'` (`E2010`).
O check dedicado `checkConstFieldAssignments`/`targetFieldIsConst` continua responsável por
campos `const`; `view` e `lend` mantêm o tratamento de ownership anterior.

`checkImmutableRootFieldWrite` trata também parâmetros de função. Parâmetros comuns têm
`bindingKind == Let` e são read-only; `var p` tem `BindingKind::Var` e permite escrita através
do parâmetro. Receivers explícitos com tipo pointer ou qualificador `lend`/`view` saem do default
read-only (o check de `view` continua em `checkAssignableOwnership`). Bare `self` é read-only,
mas `var self` permite escrita in-place nos campos.

`PerModuleSema::borrowParamType` converte `lend T`/`view T` de parâmetros livres e self explícitos para `*lend T`/`*view T`; `isBorrowParamType` reconhece esses ponteiros mesmo através de aliases. `checkOwnershipCoercion` roda antes da coerção de tipo em calls livres, chamadas genéricas, overloads e métodos:

- binding `default`/`unique` para parâmetro borrow exige `lend`/`view` no call site (`E4005`);
- argumentos já `lend`/`view`, literais e temporários não exigem anotação;
- anotação incompatível com o parâmetro reporta `E4005`;
- cada call mantém um conjunto de roots anotados; o mesmo root não pode repetir `lend` nem misturar `lend` com `view` na mesma chamada (`E4005`);
- `OwnershipCoerce` mantém o tipo do inner para inferência/overloads; o narrowing real para `*lend T`/`*view T` só acontece depois do check.

`@lengthOf` e `@ptrOf` reconhecem parâmetros borrow que referenciam slices. O HIR preserva o
tipo ABI de ponteiro para slice; LLVM e VM leem o descritor emprestado para projetar o
comprimento ou o ponteiro dos elementos.

O probe de overload usa `coerceValue` para parâmetros borrow, para que `f(lend q)` seja compatível com `fn f(p: lend P)` mesmo quando a ABI semântica é ponteiro.

O acesso `self.field` auto-derefs um receiver implícito `*Owner`: `inferField` resolve o pointee
quando o tipo do objeto é pointer, e `HirLowerModern::lowerField` emite um `HirUnaryOp::Deref`
antes de ler/escrever o campo. `self->field` continua no caminho legacy e produz o mesmo acesso.

`PerModuleSema::fieldVisible` aplica a visibilidade por field usando a `FieldMeta` paralela de
`StructType` (owner, visibility e modDepth). `pub` é sempre visível; `Private` só dentro do
ficheiro do struct; `mod`/`mod(N)`/`mod(..)` segue a mesma regra de profundidade de módulo usada
para declarations. Esta regra é consultada por `inferField`, `inferArrow` e por ambos os caminhos
de struct literal (genérico e concreto). Num literal, um field invisível é rejeitado com o mesmo
diagnóstico de accessor privado e não é contado como field necessário. Fields privados não
participam na inferência de argumentos genéricos a partir de literais.

`PerModuleSema` mantém `movedLocals_`, um dead-state lógico por corpo de função. Ao chamar um
método com `self` implícito ou `var self`, `inferMethodCall` marca a raiz do receiver como movida;
leituras posteriores do nome reportam `E4001 UseAfterMove` e escritas através de campos/índices
do local movido também. Atribuir diretamente ao nome do local (o próprio root) revive a ligação.
Para owners de implement `*char`, o receiver é passado como valor pointer e a invalidação
pós-call é suprimida: `p.method()` e `p->method()` podem ambos usar `p` sem E4001.
A exclusividade de `lend`/`view` está implementada no call site; o dead-state lógico de receivers
e o comportamento pós-chamada de funções livres continuam como antes.

O mesmo dead-state cobre o address-of `&x`: `inferUnary` regista o operand como movido
logicamente e a assinatura do pointer resultante fica num alias local por binding. Não há pass
SSA nem phi nodes. Um contador/versão por nome dentro do corpo da função é consultado em
`inferName`/`inferAssign`/`checkMovedRoot`. Atribuir directamente ao nome revive a versão nova;
escritas através do pointer não revivem o binding original. `raw` sobre index/deref continua a
ser o escape unchecked e não cria aliasing object. `pointerAliasEscapesScope` rastreia aliases
locais de pointer object, e usos que saiam do scope (return, struct/array/global/defer,
aggregados persistentes) reportam `E4008 PointerEscapesScope`. Calls por valor de argumento
pointer são tratados como borrows temporários. Pointers devolvidos por calls não são marcados
como aliases de storage local.

`@lengthOf`/`@ptrOf` são intrinsics de valor no mesmo `ExprKind::LayoutIntrinsic`. Sema tipa
`@lengthOf` como `u64` para slices, arrays e strings; `@ptrOf` devolve `*T`/`*char` e marca a
expressão como escaping quando aponta a storage local. `HirLowerModern::lowerLayoutIntrinsic`
materializa `HirLayoutIntrinsic::Which::LengthOf`/`PtrOf`, guarda o operand/type e, para string
literal, decodifica uma vez os escapes para calcular o comprimento em memória. Codegen projecta
o length/data de slices, usa `emitAddrOf` para arrays, e trata string literals como pointer
identity com length constante. Cache serializa operand/type e o comprimento na instrução compacta.

String literals têm tipo de origem `*char`. `PerModuleSema::coerceValue` aceita esse literal
directamente num destino `[]char`, atualizando o tipo da expressão para o slice; `coercesTo`
permite a conversão inversa apenas para `[]char -> *char`. `HirLowerModern::lowerCoerceToTarget`
detecciona o literal mesmo depois do retype do sema, cria o pointer literal decodificado e um
`HirMakeSlice` com `is_pointer=true`, `lo=0` e `hi=@lengthOf` do mesmo decode. Um `*char`
que não é literal não converte para `[]char`; a conversão implícita de `[]char` para `*char`
equivale a `@ptrOf` e é marcada como escaping por `E4008` quando escapa ao storage local.

`PerModuleSema::checkLiteralEscapes` corre em `inferLiteral` para literais de string e de
carácter. Decodifica o corpo com `decodeEscapes` e reporta `E0003 InvalidEscape` para um
escape desconhecido ou malformado e para um literal de carácter que não decodifique exatamente
um carácter. Isto torna o `check` (que para em `TypeChecked`) consistente com o `build`/`run`,
que de outra forma só falhavam no lowering. `decodeEscapes` aceita octal `\NNN` (um a três
dígitos), pelo que `\0` é NUL de um dígito e `\033` é ESC; antes desta alteração `\0` emitia
um único NUL e deixava os dígitos seguintes como caracteres, truncando a string no NUL.

`@canonicalType(T)` segue o caminho de `LayoutIntrinsic` no frontend, mas o sema tipa a
expressão como `u128`. A regra canónica é a função partilhada `types::canonicalTypeId`
(`src/types/type-canonical.cpp`), que deriva o id estável a partir do namespace do módulo que
define o tipo, do tipo e dos campos ordenados por tamanho; `HirLowerModern::canonicalTypeId`
apenas delega nela, e o cache usa a mesma função para canonização.
`lowerLayoutIntrinsic` materializa `HirCanonicalType`. Codegen emite o valor como constante
`i128` e o cache persiste os dois `uint64_t` do id.

`inferMethodCall` reconhece `p.Trait.method()` (AST `Call(Field(Field(p, Trait), method))`)
antes da lookup normal. Quando o receiver é um struct que satisfaz a trait/interface nomeada,
o nó intermediário `p.Trait` é marcado com o tipo concreto e o recebedor real é guardado em
`TypedMap::traitQualifiedReceiverBase`; HIR baixa esse marker como o próprio base, sem emitir um
field que não existe. A seleção de candidatos é então filtrada pelo trait: defaults do trait,
requirements e métodos impl são considerados apenas nesse trait, e interfaces estruturais também
aceitam o método concreto do owner que satisfaz a interface. Sem qualificação, `p.method()`
mantém a seleção atual e continua reportando `E2008` quando dois traits expõem o mesmo nome.

`dyn Trait`/`dyn Interface` são erguidos por `lowerCoerceToDyn`, que cria `HirMakeDyn`
(fat pointer: data pointer + vtable) e `HirVTable` com um slot por method requirement; cada slot
é o symbol do método concreto do owner que satisfaz o trait/interface (ou do default do trait,
quando não há override). Chamadas `p.method()` sobre `dyn` descem para `HirDynCall` com
`slot_index` na ordem dos requirements. Field access sobre um valor `dyn` não tem lowering:
`inferField` vê `TypeKind::Dyn` como não-struct e reporta `E3001` com "field access on non-struct
type". Este comportamento é intencional no Zith-- e a superfície pública de `dyn` só expõe
métodos. Para ler fields, use um tipo concreto ou um bound genérico `T: Interface` (que continua
a expor fields e methods no corpo).

`?dyn Trait`/`?dyn Interface` usa uma representação niche: o slot guarda o próprio fat pointer
de `dyn` e um data pointer nulo marca `null`, sem tag adicional (`codegen-type.cpp` devolve o
fat pointer para o `TypeOptional` cujo `inner` é `Dyn`). `HirLowerModern::isNicheOptional`
classifica tanto `?*T` como `?dyn T`, e todos os caminhos opcionais de HIR (condição `is null`,
payload, loop `for-in` e narrowing) tratam ambos como niche em vez de extrair o field 0. Nos
slots de escrita (`lowerCoerceToOptionalErased`) o payload concreto é erguido com
`lowerCoerceToDyn`/`lowerCoerceToOpaque` antes do `HirMakeSome`, e o codegen devolve o valor
inalterado quando o tipo do slot coincide com o do payload. Dispatch sobre um recetor `?dyn`
exige prova de não-nulo em `inferMethodCall`, que caso contrário reporta o diagnóstico dedicado
"narrow it after 'is null'" e tipa o callee como `error` para suprimir a cascata de field access.

`enum` e `union` são tratados como `struct` para efeitos de generics e métodos:
o parser aceita métodos `fn` inline no corpo, `lowerDeclarationTypes` regista os
templates com `GenericParam`, e `instantiateTypeExpr` reifica `Enum<T>`/`Union<T>`
com membros/variantes substituídos e preservação de `is_tagged`. `findMethodsForOwner`
e `inferMethodCall` resolvem receivers `TypeKind::Enum`/`TypeKind::Union` com o owner
base, e `implement Enum<T>`/`implement Union<T>` herdam params e registam
conformance como em `implement Struct<T>`. `HirLowerModern::lowerType` usa o type id
concreto, e discriminantes de enum genérico descem do tipo sema actual em vez do template.

Structs genéricas concretas têm identidade estrutural e nomeada: a reificação
central em `GenericInstantiationPass::substituteType` deriva o nome do tipo a
partir dos type args concretos e guarda esses args no `StructType` como
metadados. A resolução de métodos usa `StructType.args` no lugar de scans aos
fields ou parsing do nome, o que mantém `Entry<K, V>` correto dentro de
`HashMap<K, V>` e resolve `contains<K, V>(view self)` dentro de funções
genéricas sem `E3011`. `GenericBinding` continua a ser a fonte de verdade para
bounds, pelo que `current.key.hash()` com `K: Hashable` atravessa fields
genéricos. O cache serializa os type args em `CompactStructDef` para que a
identidade do tipo sobreviva à hidratação entre sessões. Os testes dedicados
estão em `tests/test-generic-hashmap.cpp`.

Discriminantes de enum são avaliados em `lowerDeclarationTypes`, não como literais fixos.
O evaluator percorre recursivamente literais inteiros, unários `-`/`~`, binários aritméticos,
bitwise `&.`/`|.`/`^.`, shifts e comparações, variantes anteriores do enum e referências a
`const` inteiros globais/locais. Usa aritmética alargada para detectar overflow de
`int64_t`, depois verifica o resultado contra o tipo subjacente do enum. Expressões não
constantes ou não inteiras reportam `enum variant discriminant must be a constant integer
expression`; valores fora do tipo subjacente são rejeitados com
`enum variant discriminant does not fit its underlying type '<T>'`.

`inferUnary` aceita `not` como o único unário booleano; `not` exige operando `bool`.
O parser não reconhece `!` como prefixo; `!` permanece apenas no caminho postfix
ainda por implementar e nunca é convertido em unário booleano ou HIR.

`inferBinary` trata `and`/`or` como booleanos e `xor` como booleano ou inteiro do
mesmo tipo. O lowering reutiliza `HirBinaryOp::And/Or/Xor` já suportado pelo
codegen. A fuga `raw x` é marcada em `Expression::isRawName`, ignora a
verificação de binding não inicializado, e baixa diretamente para a leitura do
binding com o mesmo tipo.

Em `inferWhen`, a primeira ilha de um `WhenGuard` é sempre um pattern contra o
subject: operandos booleans e optionals são condições, ranges comparam com o
subject, e operandos não-booleanos são convertidos em igualdade com o subject.
Alternativas `and`/`or`/`xor` dentro da primeira ilha são validadas
recursivamente, para que `(Status.Ok or Status.Error)` signifique
`status == Status.Ok or status == Status.Error`. As ilhas seguintes são guards
booleanos normais (ou optionals, com o teste implícito) e não podem combinar
com `(_)`. O formatter emite a forma canónica sem `~>`, preservando parêntesis
por ilha e alternativa.

`PerModuleSema` mantém `uninitializedLocals_` por corpo de função: bindings sem
inicializador entram no conjunto durante `inferBlock`, a primeira escrita direta
remove o local, e `inferExpr` rejeita leituras normais desse conjunto. A regra
de `let x; x = e;` continua a permitir a primeira escrita para inferir o tipo.

`checkFunctionDefaults` valida os parâmetros com `Parameter.defaultValue`: um parâmetro sem default não pode seguir um com default, e o tipo do default é verificado contra o tipo do parâmetro com `coerceValue`. A resolução de calls reutiliza `missingArgsHaveDefaults` para aceitar aridades menores em calls livres, genéricos, overloads, métodos, métodos genéricos, métodos dyn, dock e transições de state quando os parâmetros fixos em falta têm todos default.

Calls com defaults são tipados com `functionDefaultType`, que consulta o snapshot e typed map do módulo declarador para expressões reutilizadas em imports/methods; em calls genéricos o default é também sujeito a `coerceValue` contra o parâmetro instanciado.

O parser de `implement` usa `parseType()` para aceitar owners canónicos `i32`, `?char`,
`[]char` e `*char` além de nomes; `lowerImplementBlock` reporta `UnsupportedSyntax` para
qualquer outro pointer (`*i32`) e para arrays fixos (`[N]T`). `ImplementRecord` guarda
`ownerType` (a `TypeExprId`) e `owner` (a string canónica). Antes de baixar assinaturas,
`prepareImplementOwners` intera `?T`/`[]T` reais para que `self` implícito em métodos desses
owners resolva; `checkImplementBlocks` valida requirements, defaults, duplicatas,
interface-explicit, e regista conformação nominal com o `TypeId` do owner. `ownerNameOf`
devolve a string canónica para Integer/Float/Bool/Char/String/Optional/Slice/Pointer e
`findMethodsForOwner` localiza esses métodos; receivers não-struct com um método concreto do
owner seguem o caminho de method-call em vez de field access. Num owner `*char`, `self`
implícito baixa para o próprio `*char`, não para `**char`; `substituteSelf` conserva esse
comportamento nas assinaturas de requirements para que `fn first(self): char` corresponda.

O sema mantém `active_loop_labels_` enquanto infere `while`/`for`/`for-in`.
Labels são aceitas nos loops e em `break`/`continue`; um label desconhecido,
para fora, ou repetido entre loops ativos reporta `E2010`. Sem label,
`break`/`continue` usam o loop mais interno. `inferBlock` também valida
`break`/`continue` em corpos de blocos, além dos paths existentes de expression.

## HIR

`predeclareGlobalConsts` roda antes das funções e cria um `HirGlobalConst` por declaração `const` top-level:

- Nome de linkage `_zith_<module>.<name>`.
- Tipo HIR inferido do decl.
- Inicializador baixado como expressão HIR.

Referências por nome a um const global produzem `HirGlobalConstLoad` no lowering. Consts locais permanecem no caminho local de slots/allocas, imutáveis e com inicializador constante obrigatório.

Referências a um global externo produzem `HirExternalGlobalAddr`, que mantém o símbolo do linker e
o tipo do objeto. O lowering usa esse endereço para leituras e, em `extern var`, escritas.
Declarações externas não referenciadas não criam acessos HIR.

`HirLowerModern::lowerExpr` descarta `OwnershipCoerce` e baixa o inner. Em calls, um argumento anotado é baixado como endereço do place: `lowerLValueAddr` para bindings/campos/índices, ou uma alloca temporária quando o operand não tem lvalue. O parâmetro `*lend T`/`*view T` já é baixado como ponteiro. Os slots de parâmetro carregam `HirOwnership::Lend`/`HirOwnership::View` via `SraFacts`, e `SraFacts::localOfArgument`/`ownershipOfArgument` descascam a anotação para continuar acumulando factas por call argument (`SraArgEscape::Borrow` por default).

`HirLowerModern::lowerFunctionLiteral` resolve a função sintetizada pela chave de instanciação da `Expression::declaration` e emite `hir::HirVar` com o nome de linkage dessa função. O caminho de `emitVar` em codegen resolve o nome para o endereço do function pointer, por isso o literal e uma referência a uma função nomeada partilham o mesmo lowering e a mesma ABI de call indireto. Não é preciso nenhum nó HIR novo.

`HirLowerModern::lowerCall` materializa os defaults em falta como argumentos normais depois dos argumentos explícitos e antes de qualquer tail variadic/slice; o mesmo faz `HirDynCall` (com offset do receiver) e `HirStateTailCall`. Como a expressão default pertence ao snapshot do módulo declarador, `lowerVisibleDefault` troca temporariamente `current_module_`/`current_resolution_`/`current_types_` (e o contexto de instanciação genérica quando existe) para baixar a expressão no contexto certo.

`checkAssignableOwnership` bloqueia escrita através de parâmetros `*view T` tanto no caminho arrow como no dot-member auto-deref, reportando `E4004`.

`HirLoopTarget` carrega o label do loop. `lowerWhile`/`lowerFor`/`lowerForIn`
registam o target com o label; `break`/`continue` sem label usam o último
target da `loop_stack_`, e com label procuram o target correspondente.
`emitCleanupFrom` percorre a stack de fora para dentro depois de escolher a
profundidade, o que também emite o cleanup de loops interiores quando o exit
salta um alvo exterior.

`HirLowerModern::lowerWhenCondition` reconhece `ExprKind::WhenGuard`: baixa a
primeira ilha seguindo as regras de pattern (range, boolean, optional ou
igualdade com o subject) e combina as ilhas seguintes com os operadores top-
level gravados no frontend. A combinação usa a precedência `or` > `and` > `xor`
para manter o mesmo resultado da árvore binária de expressões.

### Optional em contexto booleano

Uma expressão `?T` é aceite diretamente em posição de condição (`if`, `while`,
`for (cond)`, `else (cond)` e armas booleanas de `when`) como teste implícito
`x != null`. A expressão mantém o tipo `?T` no `TypedMap`; sema usa
`inferCondition`, e o lowering materializa o teste com `lowerCondition`.
O açúcar `optional expr` foi removido do parser. `x?` deixa de ter modo booleano
em condição e passa a validar sempre propagação opcional na função atual.

`HirLowerModern::lowerOptionalCondition` baixa `?*T` para `Ne` contra
`HirMakeNone` (niche de pointer) e `?T` agregado para leitura do discriminante
em field index 1. Não gera `return null` e não altera o tipo do construto.

Extração usa `must x` (panic runtime `R10003` quando é `null`) e `raw x` (bypass
sem check, explícito e arriscado). Ambos devolvem o payload; `is null`/`not
(x is null)` continuam a ser os únicos caminhos de narrowing. Não existe nesta
iteração `?.` nem `?->`.


### Opaque tagged

Bare `opaque` não é o antigo `raw opaque` (`void*`). No frontend usa
`TypeExprKind::OpaqueTagged` e no tipo interno `TypeKind::Opaque`; o tipo HIR/codegen é
`{ *void, u32 }`, sendo o primeiro campo um ponteiro para um slot local estável e o segundo
o typeId concreto do módulo.

`registerPrimitiveTypes` regista um `opaque_type` estável por `PerModuleSema`; `lowerBareTypeExpr`
e `inferCast` devolvem esse `TypeId` em vez de internarem um opaque novo. `sameType` já não tem a
regra antiga que tornava `TypeKind::Opaque` implicitamente igual a qualquer tipo. Conversão
implícita `opaque -> T`/`T -> opaque` falha em `reportCoercionFailure` com
`implicit 'opaque' conversion is not allowed; use 'as' (or 'raw as' for an unchecked extraction)`;
os casts explícitos existentes continuam a ser os únicos caminhos aceites.

Semanticamente, `T as opaque` aceita tipos endereçáveis/copyable e não exige `&x`. O lowering
`HirMakeOpaque` spilla o valor para uma alloca local antes de construir a view, pela mesma
técnica usada por `dyn`. `opaque is T` baixa para `HirOpaqueCheck` e compara o typeId no campo
1. `opaque as T` baixa para um branch CFG que produz `Some(T)`/`None` e devolve `?T`; o ramo
`Some` usa `HirOpaqueCast` desmarcado para ler o payload do endereço. `raw opaque as T` reutiliza
o path `HirOpaqueCast` desmarcado sem qualquer check de tag.

O typeId é derivado de `moduleNamespace + ":" + HIR TypeId` com FNV-1a e é materializado como
constante i32 no cast. Não há vtable nem registry global nesta iteração. Para manter os ids
determinísticos no mesmo módulo, `HirMakeOpaque`, `HirOpaqueCheck` e `HirOpaqueCast` guardam o
mesmo HIR `TypeId` do payload; structs e outros aggregates são erradicados como um todo antes
de qualquer extração nominal/unaria.

## Codegen

`emitConstGlobals` é executado antes das funções:

1. Pré-declara todos os `llvm::GlobalVariable` com linkage `InternalLinkage` e `isConstant = true`.
2. Emite cada inicializador.
3. Se o inicializador não for um `llvm::Constant`, reporta erro de codegen.

`HirGlobalConstLoad` faz load do global pelo nome. Não há armazenamento para globals const no emissor.

`HirExternalGlobalAddr` resolve para um `llvm::GlobalVariable` de linkage externo com o nome C
declarado. Leituras carregam do objeto e escritas são emitidas apenas para globals mutáveis.
Referências repetidas ao mesmo símbolo reutilizam uma única declaração LLVM.

A VM v2 não implementa storage global externo. Se o HIR contém um acesso a esse objeto, o lowering
falha com `unsupported external global '<name>' in v2 VM`. Uma declaração importada sem referências
não impede o lowering nem a execução VM.

Parâmetros HIR com residual de slot `Lend`/`View` recebem `nocapture` no LLVM; `View` recebe também `readonly`. O valor do parâmetro em si continua a ser carregado da alloca do slot, preservando o caminho sem LLVM/legacy para o corpo.

Codegen emite `HirMakeOpaque` como `alloca T` + `store T` + bitcast `T*` para `void*` +
`insertvalue { void*, i32 }`. `HirOpaqueCheck` extrai o typeId e compara com a constante.
`HirOpaqueCast` desmarcado extrai o ponteiro e faz load do tipo pedido. O checked extraction
foi baixado em HIR para CFG, por isso não existe branch no emissor para `HirOpaqueCast.checked`.
`opaque as raw opaque` é uma variante unchecked que devolve o campo 0 do aggregate
directamente como `void*`: o lowering marca `HirOpaqueCast.returns_ptr`, e o codegen faz
bitcast do payload sem load. Isto permite comparar os ponteiros reais armazenados numa bare
`opaque` sem confundir o aggregate tagged com um ponteiro LLVM.

## Import Resolution and Platform Variants

`FrontendContext::resolveImport` mantém os caminhos literais para headers e
assets. Para importações Zith, cada search root é visitado pela mesma ordem do
plano:

1. Ficheiro regular no caminho literal.
2. `foo.<arch>.<os>.zith`.
3. `foo.<arch>.zith`.
4. `foo.<os>.zith`.
5. `foo.zith`.
6. Diretório `foo`, agregado conforme a profundidade pedida.
7. `foo/mod.zith`.

Um diretório não substitui um ficheiro regular com o mesmo caminho lógico.
Esta regra permite que `std/memory.zith` funcione como facade apesar de
`stdlib/std/memory/` conter os módulos exportados.

`targetComponents` extrai `arch` e `os` do `config_.targetTriple` com
`llvm::Triple`; sem LLVM a função devolve componentes vazios e não são geradas
variantes. `platformVariantSuffixes` constrói os sufixos por essa ordem e o
resolver só varia o último segmento do path. Quando não há ficheiro resolvido
numa importação Zith, `ResolvedImport::consideredPlatformVariants` torna o
diagnóstico mais accionável:

```text
could not resolve import 'foo'; missing generic module or matching platform variant
```

O artifact resolvido, spans e cache usam o path final (`foo.<arch>.<os>.zith`
quando vence uma variante), nunca o nome lógico da importação. O `CacheKey`
continua a incluir `targetTriple`, por isso contextos com targets diferentes
separam os artifacts mesmo quando o ficheiro genérico é igual.

## Cache e ZIRL

A versão de formato ZIRL está em 17 (`kFormatVersion` em
`src/zirl/zirl-header.hpp`). O Code section serializa:

- `Artifact.exprs` como pool de expressões ao nível do módulo.
- `Artifact.globals` como `CompactGlobalConst` com name, type e init.
- `HirFunction` com `isState`/`machineId`/`machineReturnType`/`usesTailCC` para declaracoes `state`.
- `HirFunction.variadicSliceParam` como `CompactFunction.variadic_slice_param`.
- `CompactFunction.discardable` para declarações `#[discardable]`.
- `HirSlotAttrsRecord.volatileSlot` para slots `#[volatile]`.
- `HirStateTailCall` como expressao de terminacao com `musttail tailcc` direto.

Isso mantém os ids de HIR estáveis entre módulos vazios de funções, const globals, loads por `HirGlobalConstLoad` e transitions `state`. Maquinas `state` agrupam por retorno canonico e permitem listas de parametros diferentes entre estados; codegen declara e chama essas funcoes com LLVM `tailcc` e sem contexto/`alloca` adicional.

## Variadic Slices

O parser marca `Parameter.isVariadicSlice` quando o último parâmetro usa `[...]T` e
propaga o flag para `ResolvedName.isVariadicSlice` e `HirFunction.variadicSliceParam`.
O sema trata `[...]T` como um parâmetro de slice normal, mas aceita:

- zero argumentos no tail
- um último argumento `[]T`/`[N]T` explícito, sem recolha elemento a elemento;
- qualquer quantidade de argumentos homogéneos desde `slice_index` até ao fim, recolhidos
  em `checkVariadicTailArgs`/`checkVariadicTail` pelo elemento do slice param.

`checkVariadicTailArgs` recebe o slice param e o primeiro índice de tail; a versão usada
em métodos e chamadas dyn precisa deste separador porque `call.operands` não inclui `self`
mas o `FunctionType` inclui o receiver. Calls genéricos e métodos genéricos sintetizam
`[]T` a partir do primeiro elemento do tail para `resolveArgs`, depois validam o tail com
o tipo instanciado.

Nos calls genéricos, a inferência de `resolveTypes` tenta o matching estrutural exacto em
primeiro lugar. Quando o parâmetro declarado é `?T` (ou `??T`) e o argumento não tem o mesmo
outer shape, `GenericInstantiationPass` faz uma probe só para opcionais: retira as camadas
opcionais do parâmetro, unifica o `T` resultante contra o argumento real e comita as ligações
se não houver conflito. Isto permite `fn wrap<T>(x: ?T): ?T` aceitar `wrap(3)` e `fn
nested<T>(x: ??T): ?T` aceitar `nested(5)` ou `nested(maybe)` com `maybe: ?i32`. A probe é
restrita a coerções opcionais. Outras coerções implícitas (arrays para slices, pointers para
`void`, `dyn`/`opaque`) continuam exactas nesta iteração.

No HIR, `HirLowerModern::lowerCall` detecta o variadic slice no callee e baixa o tail para
`HirArrayLiteral` + `HirMakeSlice` num slot temporário; um único argumento slice/array final
é passado pelo caminho normal com `lowerCoerceToSliceIfArray`. O mesmo algoritmo cobre calls
livres, métodos, `dyn`, state/jump/dock se o alvo tiver o flag. O code section do cache
persiste `variadic_slice_param` para hidratar o HIR sem voltar a correr sema.

## Defer e Cleanup de Escopo

`defer expr;` e `defer { ... }` registam cleanup no bloco lexical mais próximo.
O frontend produz `StmtKind::Defer`; `defer { ... }` guarda o corpo como
`ExprKind::Block` cleanup-only. O sema recolhe os `defer` do bloco, tipa depois
os bindings diretos e só então infere cada corpo adiado; `checkDeferCaptures`
rejeita capturas de bindings do mesmo bloco declarados depois quando um exit
directo (ou controlo aninhado marcado como exit) antes do inicializador pode
fazer o cleanup correr sem o binding inicializado. O corpo adiado continua a
recusar `return`/`break`/`continue`/`jump` e não contribui com o valor do
bloco. `lowerBlock` acumula `StmtKind::Defer` em `pending_defers_` e
`flushPendingDefers` baixa os corpos no fim do bloco, depois dos slots das
bindings directas; antes de `return`/`break`/`continue`/`jump` os pending defers
são emitidos primeiro. O cleanup final é `HirExprKind::Cleanup` em reverse order
antes de `ret`, branches de `break`/`continue` e `HirStateTailCall`. `state`
sem return type declarado é tratado como `void` e nunca tem tipo inferido do corpo.

## Pipelines `|>` e `do`

O frontend baixa `x |> stage` para `ExprKind::Pipe` e `x do stage` para
`ExprKind::PipeDo`; dentro de um stage o `..` é `ExprKind::PipeCurrent`. O
stage é explicitamente threaded: `x |> f(..)` equivale a `f(x)` com `x`
materializado uma vez, e `x do f(..)` executa o stage como efeito mas mantém a
chain em `x`. Não há injeção implícita de argumentos nem propagação automática
de `?`/`!`/tags.

`PerModuleSema::inferPipe` guarda o tipo da source, infera o stage com um
contexto `PipeCurrent` e rejeita `..` fora desse contexto. `|>` exige
exatamente um `..`; `do` aceita zero ou um `..`. `do { ... }` é cleanup-only:
não produz um valor e não pode transferir controlo com `return`/`break`/
`continue`/`jump`. O lowering
`HirLowerModern::lowerPipe` materializa a source num slot, baixa o stage com
`HirPipeCurrent` para esse slot e emite `HirPipe`. `do` com um bloco sem valor
usa um `HirCleanup` vazio como stage sintetizado para que o efeito e a ordem
continuem visíveis no HIR e no codegen; codegen executa o stage e depois volta
a ler o slot para preservar o valor da chain.

For-in usa o protocolo canonico `next(self): ?T`: `null` é o fim da iteração e `Some(T)` é um
elemento. `next(self): ??T` suporta iteradores com elementos opcionais; o loop variable é `?T`
e só o `None` exterior termina. O HIR chama `next` no header, ramifica por um branch sobre o
optional (`HirMakeNone`/tag field para optionals de valor, igualdade a `null` para optionals de
ponteiro) e extrai o payload com `HirField`/load. O protocolo legacy tagged union
`next(self): { T, End }` continua aceite durante a migração, emitindo `HirUnionCheck`/`HirUnionCast`.

### Range literal, `in` e `Contains`

O frontend representa `lo..hi`, `lo>..hi`, `lo..<hi` e `lo>..<hi` como
`ExprKind::Range` com dois operandos e as flags `openAtLo`/`openAtHi`. Os
bounds ficam brutos no AST e no HIR; o lowering não pré-computa `lo + 1` ou
`hi - 1`.

`in` é um operador binário normal de precedência de comparação. O sema
resolves o RHS por ordem:

- Um `ExprKind::Range`: tipos iguais em `inferContains`, sem método chamado.
  O HIR baixa para `(value > lo or value >= lo) and (value < hi or value <=
  hi)` conforme `openAtLo`/`openAtHi`.
- Um tipo com método `contains(self, value): bool`: `inferContains` grava a
  chamada em `TypedMap::containsCall` e o HIR emite `contains(rhs_addr, value)`,
  passando o receiver por endereço e o valor por valor.

`TypedMap::forInRangeLiteral` marca os `ForIn` sobre ranges inteiros. O
`lowerForIn` usa um contador de passo `1`, guarda `lo`/`hi` em slots e aplica
as comparações abertas/fechadas no header e no passo inicial de bound aberto.
Ranges float passam pelo sema como valores `in`/`Contains`, mas são rejeitados
em `for (x in float_range)` com um diagnóstico único. O `Iterator` existente
com o caminho `next` permanece intocado para tipos não-range.

Funções com retorno não-void usam análise de terminação por caminhos: um corpo pode terminar
sem `return` apenas quando o último valor do bloco tem o tipo certo ou quando todos os caminhos
terminam (valores finais, `if`/`else` completo, `when` com default, `jump`, loops infinitos sem
`break` directo). Fallthrough continua a ser erro diagnosticado; não é criado `null` automático
para `?T`.

## Testes

Os seguintes testes cobrem a iteração:

- `test-frontend`: bindings `let`/`var`/`const`, rejeição de `global`/`mut`/ownership/tags, const fields.
- `test-frontend`/`test-frontend-modern-pipeline`: parsing de `extern var`/`extern const`, aliases,
  tipos importados de C, endereços HIR e mutabilidade de globals.
- `test-cinterop`: importação de objetos externos diretamente declarados no header e typedefs
  alcançáveis, com exclusão de globals `static` ou transitivos.
- `test-frontend`/`test-formatter`: sintaxe nova de `when` sem `~>`, guards com mais de uma ilha, alternativas `and`/`or`/`xor` no pattern e deprecation `W1008` da seta legada.
- `test-sema`: valida const global/local, campos const, propagação de imutabilidade em structs/unions, discriminantes constantes de enum e atribuições proibidas.
- `test-hir-lower-modern`: `HirGlobalConst`, `HirGlobalConstLoad` e valores de enum calculados a partir de expressões.
- `test-enum-union-generics`: métodos inline em enums/unions genéricos, reificação,
  `implement E<T>`/`implement U<T,U>`, `dyn` de instâncias concretas, aridade/`Self`
  e raw casts lowered post-instantiation.
- `test-generic-hashmap`: `HashMap<K, V>` checked via `zithc check`, struct
  reificação com type args concretos, propagação de bounds e round-trip de
  cache para structs genéricas.
- `test-codegen`: execução runtime de um const global e de uma maquina de estados com `musttail tailcc`, incluindo parâmetros divergentes.
- `test-codegen`: leitura/escrita de globals C e uso de `stdout` importado via `stdio.h`.
- `test-vm-v2`: imports externos não usados continuam permitidos; acessos referenciados falham com
  um diagnóstico que identifica o símbolo.
- `test-cache`/`test-zirl-sections`: pool de expressões, globals e state machine metadata persistidos.
- `test-memory-qualifiers`: lend/view aceites; anotações de call `E4005`/`E4007`, exclusividade por call e views read-only; unique/share/belong e mut rejeitados.
- `test-sema`/`test-codegen`: `var p`/`var self`, `self.field` com auto-deref e dead-state `E4001` de receivers pós-método.
- `test-sema`: overload/generic com parâmetros lend/view e mismatch de tipo que continua a reportar `E2007` sem mascarar `E4005`.
- `test-hir-lower-modern`: parâmetro livre `lend` baixa para ponteiro e call passa endereço do binding.
- `test-codegen`: parâmetro livre `lend` muta o binding do chamador; `view` lê sem escrever.
- `test-frontend-context`: ordem de resolução de platform imports, arch-only,
  os-only, fallback genérico, diagnóstico de missing e separação de cache por
  target.
- `test-frontend-modern-pipeline`: `export foo` re-exporta o ficheiro
  platform-specific resolvido.

Para regressões, usar a suíte completa:

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --build build --target fmt-check
```

Se LLVM não estiver disponível, validar parser/sema/HIR e reportar explicitamente que codegen/cache com LLVM não foi executado.
