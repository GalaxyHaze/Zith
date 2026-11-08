# Rascunho de Apresentação de Zith--

Estado: rascunho. Rever depois de concluir as correções e extensões planeadas
para o compilador. Este texto ainda não é um anúncio de lançamento.

## Texto Principal

Zith-- é uma linguagem de sistemas estaticamente tipada, atualmente em beta.
Explora uma forma direta de estruturar código de sistemas: tornar visíveis os
passos importantes sem exigir cerimónia quando ela pouco acrescenta. O público
principal são pessoas que já conhecem linguagens como C++, Zig ou Rust e estão
abertas a outra forma de organizar o código. Iniciantes também são bem-vindos.

O projeto começou como uma ideia para a minha PAP, o projeto final do curso.
Por volta de novembro ou dezembro de 2024, decidi criar uma linguagem de
programação. Antes de começar a implementação, passei algum tempo a explorar
os conceitos iniciais. Uma das primeiras perguntas foi se ideias de
data-oriented design (DOD) e entity-component systems (ECS) poderiam fazer
parte de uma linguagem de propósito geral.

O primeiro nome foi Cintra, inspirado pela família C. Kalidous foi outro nome
inicial, escolhido simplesmente porque gostei dele. A linguagem começou com
uma forte influência de C++, mas foi evoluindo com o estudo de outras
linguagens, pesquisa, discussão e iteração.

Zith e Zith-- são linguagens irmãs com propostas distintas. Zith-- é a
linguagem mais simples e direta. Zith explora uma proposta mais ambiciosa. O
compilador `zithc` deste repositório tem como alvo Zith--, por isso as ideias
documentadas para Zith não devem ser entendidas como funcionalidades de
Zith--.

Se tens interesse em linguagens de sistemas, consulta a
[especificação de Zith--](../Zith--.md), o
[estado atual do compilador](../impl-status.md) e o
[repositório do projeto](https://github.com/GalaxyHaze/Zith). Comentários
sobre a linguagem e a sua direção são bem-vindos.

## Versão Curta

Zith-- é uma linguagem de sistemas estaticamente tipada, atualmente em beta.
Explora uma forma direta de estruturar código de sistemas, mantendo visíveis
os passos importantes sem tornar cada operação verbosa. Começou como um
projeto final de curso e partiu de uma pergunta sobre como combinar uma
linguagem de propósito geral com ideias de data-oriented design e
entity-component systems. Zith-- e Zith são linguagens irmãs com propostas
distintas. O compilador deste repositório tem como alvo Zith--.

## Notas Editoriais

- A data de novembro ou dezembro de 2024 é uma recordação do autor. Manter a
  formulação aproximada, a menos que surja um registo datado.
- Manter a influência de C++ na história, mas não descrever Zith-- como uma
  derivação de C++ ou como uma alternativa a Rust.
- Não usar "Explicit where it matters, Easy where it doesn't" como slogan
  público. A frase pode orientar internamente a comunicação sobre Zith--.
- Separar as propostas de Zith das afirmações sobre o compilador atual de
  Zith--.
- Antes de publicar, confirmar a descrição de beta e cada afirmação sobre
  implementação em `docs/impl-status.md`.
