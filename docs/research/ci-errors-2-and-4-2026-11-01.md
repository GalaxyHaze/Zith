# CI: erros 2 e 4

Investigação do run de CI `41343957704`, executado em 2026-11-01 no commit
`04238a6d4ec8da76a9c61765f10e0f24b34cff7d`. Esta nota distingue evidência
observada de causa inferida e propõe validações antes de alterar o workflow ou
a semântica.

Estado: as correções propostas para os dois erros foram implementadas. O erro 2
foi corrigido e verificado localmente. O erro 4 foi corrigido no workflow e a
receita foi confirmada por um run verde do pipeline de release. O run de CI
original precisa de uma nova execução para confirmar o resultado no GitHub.

## Erro 2: primeira escrita num `let` tipado

### Evidência

O teste `test-examples` falha em Debug e ASan/UBSan na linha 14 de
`examples/bindings-advanced.zith`, com E2010:

```zith
let q: i32;
q = 42;
```

O contrato em `docs/Zith--.md` permite esta primeira escrita e rejeita as
seguintes. O exemplo tem um único `main` com tipo de retorno explícito, pelo
que não precisa da inferência do tipo de retorno para executar este caso.

Em `sema-modern.cpp`, `SemaPipeline::inferFunctionReturnTypes()` chama
`probeFunctionCallTargets()` para cada função antes de `checkExpressions()`.
`probeFunctionCallTargets()` limpa `typeInferredByAssignment_` antes de inferir
o corpo, mas não limpa esse vetor ao sair. Durante a sondagem de `main`, a
primeira escrita de `q` é adicionada ao vetor por `inferAssign()`. A etapa
seguinte limpa os resultados provisórios do mapa de tipos, mas não limpa esse
vetor. Como `main` tem retorno declarado, não passa por
`inferFunctionReturnType()`, o outro caminho que limpa o vetor. Assim, a
análise real encontra `q` já registado, classifica a escrita legítima como
repetida e reporta E2010.

### Diagnóstico

A causa está confirmada no fluxo de estado entre as passagens: a lista usada
para contar a primeira escrita é estado temporário da sondagem, mas sobrevive
à sondagem e contamina a análise que emite diagnósticos. O problema não é a
remoção de módulos de `stdlib/`.

### Correção proposta

Tratar `typeInferredByAssignment_` como estado local à análise de uma função e
de uma passagem. Como correção mínima, limpar a lista ao sair de
`probeFunctionCallTargets()` ou antes da passagem definitiva de
`checkExpressions()`. A opção mais robusta é guardar e restaurar esse estado
em cada sondagem, ou centralizar o reset no limite entre passagem provisória
e definitiva. A lista deve continuar ativa dentro de uma única passagem
definitiva para que uma segunda atribuição no mesmo binding continue a ser
rejeitada.

Adicionar regressões que cubram `let q: i32; q = 42;` seguido de uma leitura,
`let q; q = 42;` e uma segunda escrita distinta que deve falhar. Incluir uma
função com retorno explícito para cobrir o percurso exato do CI e outra com
retorno inferido para proteger o reset existente.

## Erro 4: link estático para MUSL com LLVM

### Evidência

O job `cross-builds (web, false)` instala `llvm-18-dev` no runner Ubuntu e
compila com Zig `0.13.0` para `x86_64-linux-musl`. A configuração pede link
estático, exige LLVM e não define `CMAKE_SYSROOT` nem restringe a procura de
headers, bibliotecas e pacotes ao alvo. A configuração encontra headers e
archives LLVM do host em `/usr/lib/llvm-18`.

O linker reporta símbolos indefinidos como
`std::__cxx11::basic_string<...>::_M_create` e
`std::__throw_length_error`, referenciados por objetos dos archives LLVM em
`/usr/lib/llvm-18/lib`. O log deste run não mostra símbolos `std::__1` nem
falhas de resolução de APIs LLVM como `TargetRegistry` ou `IRBuilderBase`.

GCC documenta `std::__cxx11` como namespace da ABI nova de libstdc++ e diz que
referências indefinidas a esses símbolos costumam indicar objetos compilados
com configurações ABI diferentes. Neste log, a evidência direta é que os
archives LLVM do host precisam de símbolos da implementação libstdc++ que o
link MUSL não resolveu. Isso é compatível com runtime C++ ausente ou
incompatível, mas não identifica sozinho qual runtime o driver Zig selecionou
nem prova uma incompatibilidade entre os objetos Zith e a API LLVM.

### Diagnóstico e limite da prova

A evidência confirma que o cross-build usa archives LLVM do host e falha ao
resolver dependências de runtime C++ requeridas por esses archives. A hipótese
mais forte é uma incompatibilidade entre o runtime C++ do link MUSL e o
runtime usado para construir o LLVM, ou a ausência da biblioteca de runtime
correspondente na linha de link. O log não permite distinguir essas opções:
as definições nos archives, o comando final de link e as opções efetivas do
driver Zig ainda não foram inspecionados. Também não foi feita uma reprodução
local do job.

O workflow atual não demonstra que o build WASM falhou. O job de runtime WASM
foi cancelado depois da falha MUSL e os passos WASM foram ignorados.

### Correção proposta

Usar uma instalação LLVM e respetivas dependências construídas para
`x86_64-linux-musl` com o runtime C++ que o link final realmente usa.
Configurar uma toolchain CMake com o sysroot do alvo,
`CMAKE_FIND_ROOT_PATH_MODE_LIBRARY`, `INCLUDE` e `PACKAGE` limitados ao alvo,
e `LLVM_DIR` apontado explicitamente para essa instalação. Os executáveis
auxiliares necessários durante o build continuam a ser ferramentas do host.
A documentação CMake descreve `CMAKE_SYSROOT` e estes modos de procura como
parte da configuração de cross-compilation.

Há uma ressalva ao escolher libc++: a documentação do projeto declara suporte
oficial em Linux apenas com glibc, não com MUSL. Portanto, selecionar libc++
para MUSL requer validação explícita. A alternativa é construir o LLVM dentro
de um ambiente MUSL com uma toolchain C++ suportada nesse ambiente, e usar o
mesmo runtime no build do Zith. Zig disponibilizar MUSL para o target não
torna automaticamente os archives LLVM instalados no host compatíveis.

Não tratar `-lstdc++` nem `_GLIBCXX_USE_CXX11_ABI` como correção isolada. O
segundo controla a dual ABI de libstdc++, não converte objetos entre
libstdc++ e libc++, nem torna archives construídos para o host compatíveis com
MUSL. Só adicionar uma biblioteca C++ ao link depois de confirmar que foi
construída para o alvo MUSL e é a mesma implementação/ABI usada para compilar
o LLVM.

### Validação proposta

1. Reexecutar o job com build/link verbosos e guardar o comando final de link.
2. Capturar `zig c++ -###` para confirmar headers, biblioteca C++ selecionada,
   sysroot e caminhos de procura.
3. Inspecionar, com `llvm-nm -C`, as referências e definições nos archives
   LLVM, em particular os símbolos `TargetRegistry::lookupTarget` e
   `IRBuilderBase` mencionados pelo linker.
4. Reconfigurar contra a instalação LLVM compatível com MUSL e verificar que
   não são selecionados arquivos de `/usr/lib/llvm-18` do host.
5. Exigir que o link estático MUSL e o teste de execução do artefacto passem
   antes de considerar este alvo validado.

## Impacto no release

Este run permanece vermelho: o link MUSL falhou e os testes Debug e
ASan/UBSan falharam. O job Release está marcado como cancelado após a falha de
Debug, e a anotação do GitHub também reporta exit code 8 no seu processo de
testes. Portanto, este run não valida um release. Há outras falhas no conjunto
de testes que precisam de diagnóstico independente. Esta investigação não
recomenda restaurar módulos removidos de `stdlib/`.

## Fontes

- [Run de CI 41343957704](https://github.com/GalaxyHaze/Zith-Lang/actions/runs/41343957704)
- [Workflow no commit do run](https://github.com/GalaxyHaze/Zith-Lang/blob/04238a6d4ec8da76a9c61765f10e0f24b34cff7d/.github/workflows/ci.yml)
- [Orquestração semântica no commit do run](https://github.com/GalaxyHaze/Zith-Lang/blob/04238a6d4ec8da76a9c61765f10e0f24b34cff7d/src/sema/sema-modern.cpp)
- [Sondagem e inferência semântica no commit do run](https://github.com/GalaxyHaze/Zith-Lang/blob/04238a6d4ec8da76a9c61765f10e0f24b34cff7d/src/sema/sema-control.cpp)
- [Atribuições no commit do run](https://github.com/GalaxyHaze/Zith-Lang/blob/04238a6d4ec8da76a9c61765f10e0f24b34cff7d/src/sema/sema-assign.cpp)
- [Exemplo que falha no commit do run](https://github.com/GalaxyHaze/Zith-Lang/blob/04238a6d4ec8da76a9c61765f10e0f24b34cff7d/examples/bindings-advanced.zith)
- [GCC: dual ABI e diagnóstico de `std::__cxx11`](https://gcc.gnu.org/onlinedocs/libstdc++/manual/using_dual_abi.html)
- [libc++: plataformas Linux oficialmente suportadas](https://libcxx.llvm.org/#platform-and-compiler-support)
- [CMake: toolchains e cross-compilation Linux](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html)
- [LLVM: opções de build da biblioteca C++](https://llvm.org/docs/CMake.html)
- [Zig 0.13: suporte MUSL no target selecionado](https://ziglang.org/download/0.13.0/release-notes.html#musl-1-2-5)
