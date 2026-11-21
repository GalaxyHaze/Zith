# Demo do comando `zithc test`

Executa a partir da raiz do repositório:

```bash
./build/zithc --include stdlib test examples/test-command-demo
```

O comando descobre os ficheiros `.zithm` em `tests/`, executa cada função
marcada com `#[test]` e mostra um resumo. Um teste falha de propósito em
`intentional-failure.zithm`, por isso a execução completa termina com código
`1` e mostra a localização do `@assert` que falhou.

Para ver apenas os testes aprovados:

```bash
./build/zithc --include stdlib test --filter math:: examples/test-command-demo
```

Para mostrar também os logs dos testes aprovados:

```bash
./build/zithc --include stdlib test --filter math:: --verbose examples/test-command-demo
```

Os logs dos testes falhados são mostrados por omissão. `--verbose` também
mostra os logs dos testes aprovados.
