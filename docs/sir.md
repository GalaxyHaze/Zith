# SIR: Descrição Atual

## Contexto

O SIR vive em `src/common/sir` e é a IR arena-backed usada pela infraestrutura de
frontend e pelo backend experimental de codegen. Ele não depende dos geradores: é
código C++ escrito à mão, com `SirBuilder`, `Module`, `Function`, `Scope`, `Block`,
`Value`, `Variable`, `Type` e `toolkit::sir::flat`.

Nesta branch, o SIR está propositalmente mais avançado que o backend. Isso é
intencional: o desenvolvimento da Zith está apenas começando e a VM atual é
considerada desatualizada e será substituída. Portanto, `src/common/sir` descreve o
contrato de IR que o codegen deve consumir no futuro, e não o conjunto que a VM já
executa hoje.

## O que o SIR representa

### Módulo

`Module` contém um nome interned, as funções declaradas e os tipos agregados criados
na arena. A API de declaração cria, por função, um escopo raiz e um bloco raiz
automaticamente.

### Tipos

Os tipos escalares são:

- `Void`
- `Bool`
- `Char`
- `I1`, `I8`, `I16`, `I32`, `I64`
- `F32`, `F64`

Também existem tipos compostos:

- `Array`: elemento + comprimento
- `Slice`: elemento
- `Pointer`: tipo apontado
- `UserDefined`: tipo nomeado através de `nameId`;

`bool` e `char` são classificados como inteiros pelo SIR para as verificações
comuns, e `isNumeric()` cobre inteiros e floats.

### Função

`Function` representa:

- nome, tipo de retorno e assinatura de parâmetros
- escopos encadeados
- variáveis declaradas
- stream de valores
- instruções terminadoras
- lista ordenada de basic blocks com `baseBlock`

A construção mantém um `currentScope` e um `currentBlock`. Valores são anexados ao
bloco atual; `ret`, `br` e `condBranch` anexam o terminator ao bloco.

### Escopo e variáveis

`Scope` é o dono de variáveis e da construção de valores. `Variable` carrega nome,
tipo, mutabilidade, initializer opcional, função, escopo e bloco. O SIR suporta
variáveis mutáveis e imutáveis, e `verify` recusa store em variável imutável.

### Operandos

`Operand` pode referenciar:

- um `Value` existente
- uma `Variable`
- um literal inteiro
- um literal float

Quando o tipo não é informado, literais inteiros são inferidos como `i32` e
literais float como `f64`.

### Valores e opcodes

O stream de valores suporta:

- `Constant` e `Param`
- aritmética numérica: `Add`, `Sub`, `Mul`, `Div`, `Rem`
- bitwise e shifts inteiros: `BitAnd`, `BitOr`, `BitXor`, `Shl`, `Shr`
- comparações numéricas: `Eq`, `Ne`, `Lt`, `Le`, `Gt`, `Ge`
- memória: `Load`, `Store`
- chamadas: `Call`

O resultado das comparações é `i1`. Aritmética, bitwise e shifts exigem operandos
do mesmo tipo; comparações aceitam tipos numéricos.

### Controle de fluxo

Cada bloco termina com um dos seguintes:

- `Return`, com valor ou `void`
- `Branch`, com um alvo
- `CondBranch`, com condição `i1` e dois alvos

Control flow não aparece dentro do stream de valores; fica no terminator do bloco.

### Chamadas

O SIR suporta chamadas:

- por referência a `Function`
- por nome interned, resolvido para uma função do mesmo módulo

`CallArgs` tem capacidade máxima de 8 argumentos. `verify` confere aridade, tipos,
mesmo módulo e consistência entre `callee`, nome e assinatura.

### Memória

`Load` e `Store` operam sobre variáveis. A IR aceita larguras escalares:
`Bool`, `Char`, todas as larguras inteiras suportadas e `F32`/`F64`.

A verificação exige que o endereço de memória seja um valor de variável válido da
mesma função, que load/store use largura suportada, e que store não atinja variável
imutável.

## Camada flat e serialização

`src/common/sir/flat` transforma o grafo arena/ponteiros em uma representação por
índices estáveis:

- `FlatModule`
- `FlatFunction`
- `FlatBlock`
- `FlatValue`
- `FlatVariable`
- `FlatType`
- `FlatTerminator`

Nessa camada, tipos, variáveis, operandos, endereços, argumentos, blocos e callees
são índices. Todo o opcode SIR atual é preservado, incluindo memória, chamadas e
control flow.

A serialização:

- usa little-endian determinístico
- usa o magic/version `ZCTSF1`
- serializa e re-interna nomes
- valida bounds, opcodes conhecidos, forma do terminator, índices de tipo e bytes
  restantes na desserialização

## Relação com o codegen e a VM atual

O SIR completo já é representável e verificável na IR e na camada flat. O backend
`src/codegen` é experimental e atual ainda suporta apenas um subconjunto de
execução:

- tipos scratch: `i32`, `i64`, `f64`
- aritmética emitida: `Add`, `Sub`, `Mul`
- memória: `Load` e `Store` são usados para slots de variáveis
- retorno: `RetVoid`, `RetI32`, `RetI64`, `RetF64`

O flatten do codegen rejeita hoje:

- `Load` como valor explícito
- `Call`
- `Branch`
- `CondBranch`
- `Div`, `Rem`
- bitwise e shifts
- comparações
- larguras fora de `i32`/`i64`/`f64`

A VM existente está desatualizada e não deve ser tomada como referência da IR. O
objetivo desta branch é desenvolver Zith usando o SIR como contrato de IR, e
substituir/reescrever o caminho de codegen e VM a partir dele.

## Verificação recomendada

```bash
cmake --build build --target sir-demo sir-test sir-flat-test sir-flat-demo -j
ctest --test-dir build -R 'sir' --output-on-failure
```

Os testes `sir-basics` e `sir-flat-basics` cobrem construção, verificação, memória,
chamadas, blocks, branches, preservação de opcodes flat, round-trip binário e
streams malformados.

## Limites e fronteira

`src/common/sir` é runtime portátil de IR e não deve virar frontend. Novos opcodes
ou mudanças de layout devem ser checados contra consumidores, principalmente
`src/codegen`. Modificações em `src/symbols/`, `src/common/import/`, geradores e
`tools/rules_kit/` continuam protegidas.
