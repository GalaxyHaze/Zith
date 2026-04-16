# Lith (Tokenizer stage)

Este diretório é uma cópia simplificada da ideia do Zith, limitada ao estágio de **tokenizer**.

## O que já faz

- Lê um arquivo fonte passado por argumento.
- Tokeniza o conteúdo.
- Imprime os tokens no formato:

```text
linha:coluna [Tipo] lexema
```

## Build

```bash
cmake -S lith -B build-lith
cmake --build build-lith
```

## Uso

```bash
./build-lith/lith caminho/arquivo.lith
```
