# SIMD, Intrinsics e Blocos Assembly

Decisão de orientação registada a 2026-08-06. SIMD não deve entrar por blocos
assembly. O caminho recomendado é intrinsics opt-in cobertas por LLVM primeiro,
tipos vetoriais depois.

## Contexto

- `@sizeOf`, `@alignOf` e `@offsetOf` já funcionam como
  `ExprKind::LayoutIntrinsic`; a mesma lista inclui vários nomes de fibonacci
  que hoje não têm semântica (`fields`, `hasTrait`, `allocate`, `pack`, ...).
- Conhecido: `@sizeOf` em condição `if` quebra codegen com `E5001 failed to
  emit the terminator of a block in function 'main'`. O HIR fica sem instrução
  para o `HirLayoutIntrinsic` e `lowerIf` cria um `HirBranch` sem condição. O
  mesmo falha aparece em `tests/test-codegen.cpp` e `examples/arrays.zithm` no
  worktree atual e não está relacionado com a correção `opaque as raw opaque`.
  Para reparar, `lowerLayoutIntrinsic`/`HirLayoutIntrinsic` têm de ser
  materializados no HIR antes de serem usados como operando binário/condição.
- O roadmap regista `F-10 @sizeOf/@intrinsic` e `F-17 reflection intrinsics`
  como trabalho em aberto.
- O codegen é LLVM com targets x86-64 e WebAssembly registados.
- O sistema de tipos ainda não tem vetores SIMD (`<4 x f32>` ou equivalente);
  adicionar intrinsics SIMD antes de tipos vetoriais é prematuro.
- O HIR é a fronteira estável entre semântica e codegen, por isso qualquer
  nova intrinsic ou bloco assembly tem de ter representação própria ali.

## Decisão

1. Implementar intrinsics C/SIMD sem assembly como mecanismo principal de
   acesso a instruções nativas.
2. Começar por intrínsecas escalares cobertas por LLVM e semanticamente
   seguras: popcount, count leading/trailing zeros, bit-reverse, byte swap e
   rotates. Estas não precisam de tipos vetoriais novos.
3. Adicionar tipos vetoriais só depois destas intrinsics escalares estarem
   estáveis, para que SIMD tenha um modelo de tipos claro e portável.
4. Blocos assembly ficam como escape de último recurso e explícito, nunca como
   porta de entrada para SIMD.

## Porquê não assembly para SIMD

- Assembly inline é target-specific, exige clobbers e constraints, e complica
  o modelo de módulos portáveis do Zith.
- WASM não suporta assembly inline clássico; intrinsics LLVM são mais fáceis
  de mapear para WASM quando existir equivalência.
- `asm` cru no HIR e no cache/ZIRL obrigaria a uma superfície nova e
  arriscada sem benefício para o roadmap de SIMD.

## Passos futuros sugeridos

1. Adicionar `HirExprKind::Intrinsic` separado de `LayoutIntrinsic`, com nome,
  args e tipo de retorno.
2. Criar tabela de intrinsics conhecidas apenas para targets LLVM onde a
  intrinsic existe; diagnóstico de unsupported no restante.
3. Emitir via `llvm::Intrinsic::getDeclaration` + IRBuilder, validando tipos
  no sema e não no codegen.
4. Depois, desenhar tipos vetoriais (`Vec<T, N>` ou análogo) e mapear intrinsics
  SIMD reais em cima desse modelo.
5. Não usar `asm!`/`__asm__` antes de existir uma necessidade concreta de
  instruções sem intrinsic LLVM estável.

## Decisão 2026-09-14 - Machine intrinsics e `unsafe asm`

Design stress-tested para full Zith (fora do Zith-- atual). O caminho da nota
original mantém-se para SIMD; esta secção fixa a camada genérica de acesso a
instruções nativas.

## Catálogo portável

- A superfície é um catálogo canónico pequeno de builtins `@nome()`.
- Começa com `@fence()`, `@pause()`, `@timeStamp()` e `@byteSwap<T>(v)`.
- Cada builtin tem um contrato semântico; um mapeamento só vale se cumprir a
  mesma promessa, não se for apenas “instrução parecida”.
- Atomics, `Atomic<T>` e `#volatile` ficam fora deste desenho por agora.

## Segurança e localização no pipeline

- Todos os builtins de máquina são `unsafe`.
- Instruções únicas de CPU não entram em `@x86.*`/`@arm.*`; vão para
  `unsafe asm {}`.
- Existe um node HIR próprio novo, tipo `HirMachineBuiltin`, com nome canónico
  e contrato. O código atual só tem `HirLayoutIntrinsic`/`HirCanonicalType` e
  ainda não usa `llvm::InlineAsm`.
- O sema valida tipos e availability do builtin contra o `targetTriple`, antes
  da lowering, para que `zithc check`/LSP falhem sem LLVM. O codegen só emite.
- Se a target não tem equivalente com a mesma promessa, diagnóstico de erro com
  o contrato violado e a alternativa `unsafe asm`.

## Mapeamento alvo

- `@fence` -> x86 `mfence`, AArch64 `dmb ish`.
- `@pause` -> x86 `pause`, AArch64 `yield`.
- `@timeStamp` -> x86 `rdtsc`, AArch64 `cntvct_el0`.
- `@byteSwap<T>` -> LLVM bswap/rev.
- WASM: recusa `@fence`, `@pause` e `@timeStamp` por agora; `@byteSwap<T>` fica
  universal.

## `unsafe asm`

- Full Zith only.
- Forma: `unsafe asm { template, inputs, outputs, clobbers }`.
- Não há mapeamento implícito de nomes Zith dentro do template; entradas e
  saídas são explícitas e ligadas a variáveis/registos.
- O dialecto continua em config (`asm = "x86_64_intel"`).

## Deltas por plataforma

- Pequenas diferenças usam `@OS`/`@arch` em full Zith, por exemplo
  `const API_CODE = when (@OS) { ("linux") 455600, ("windows") 7 }`.
- Constantes: delta é o valor; funções: delta troca o corpo todo e a assinatura
  é validada contra o generic; declarações novas vivem em módulos platform.
- Apenas a versão final mesclada é sema/codegened.
- Sem arm nem `(_)`, a `when (@OS)` falha. `(_)` é permitido se fizer sentido.
- Imports por plataforma mantêm a ordem atual
  `foo.<arch>.<os>.zithm`, `foo.<arch>.zithm`, `foo.<os>.zithm`, `foo.zithm`;
  não há merge automático entre variante e generic.

## Suporte documentado

- Criar `docs/machine-intrinsics.md` como matriz por target (x86-64, AArch64,
  WASM), com contrato, instrução emitida, testes e alternativa asm.
- `std.arch` é definido uma única vez com nomes e contratos; a seleção por
  target fica no compilador, não em duplicações por ficheiro platform.
