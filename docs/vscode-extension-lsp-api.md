# Zith LSP para a Extensão VS Code

Este documento é o guia de handoff entre o `zith-lsp` e a
`zith-extension/vs-code`. Descreve a API atual do servidor, o que a extensão
VS Code já consome e o que deve ser implementado quando a extensão for passada
para esta API.

Está dividido em seis partes:

1. [Contrato do servidor](#1-contrato-do-servidor): transporte, inicialização
   e capacidades do `zith-lsp`.
2. [Métodos LSP](#2-metodos-lsp): pedidos e notificações usados pela
   experiência de editor.
3. [Comandos e notificações Zith](#3-comandos-e-notificacoes-zith): operações
   específicas da linguagem.
4. [Estado atual da extensão](#4-estado-atual-da-extensao): o que já existe e
   está testado no VS Code e o que continua em falta.
5. [Referência testada](#5-referencia-testada): nomes de comandos,
   configurações, evidência de teste e limitações conhecidas.
6. [Exemplos de configuração](#6-exemplos-de-configuracao): snippets JSON-RPC.

O estado em baixo reflete a extensão `0.4.0` e o servidor `zith-lsp`
`0.1.0` na revisão de baseline registada no roadmap de beta. As capacidades
listadas na secção 1 são o contrato do servidor. As secções 4 e 5 descrevem o
que a extensão consome hoje e onde ficam as lacunas.

## 1. Contrato do servidor

### 1.1 Lançamento

O `zith-lsp` é um processo que fala LSP sobre `stdio`. Não aceita opções de
linha de comandos para `--stdlib` ou `--root`. O caminho do stdlib e a raiz do
workspace devem ser configurados no `initialize`.

```text
zith-lsp
```

A extensão VS Code deve:

- lançar o binário com `stdin` e `stdout` separados, nunca com PTY
- usar `vscode-languageclient` com `transport: 0` (stdio)
- associar o servidor ao idioma `zith` e a esquemas `file`
- configurar o stdlib com `initializationOptions.stdlibPath`
- aguardar `client.start()` antes de expor comandos LSP

### 1.2 Framing e limites

O servidor usa o framing LSP:

```text
Content-Length: <bytes UTF-8>\r\n
\r\n
<JSON>
```

Mensagens de entrada estão limitadas a 64 MiB. JSON inválido é registado em
`stderr` e ignorado. O servidor não responde com `-32700`. Cabeçalhos
desconhecidos são ignorados.

A extensão não precisa de implementar o framing porque
`vscode-languageclient` já o faz, mas deve manter o `outputChannel` dedicado à
leitura de `stderr`.

### 1.3 Inicialização

O único método de configuração Zith suportado no `initialize` é:

```json
{
  "initializationOptions": {
    "stdlibPath": "/abs/path/to/zith/stdlib",
    "zith": {
      "performance": {
        "metrics": false,
        "diagnosticsDebounceMs": 75
      },
      "frontend": {
        "enabled": true,
        "warmupStdlib": true,
        "maxWorkers": 0,
        "statusNotifications": true
      }
    }
  }
}
```

| Campo | Tipo | Padrão | Uso na extensão |
|---|---|---:|---|
| `stdlibPath` | string | `""` | Deve apontar para o stdlib empacotado ou configurado pelo utilizador. |
| `zith.performance.metrics` | boolean | `false` | Ativar para depuração de latência; faz o servidor emitir `zith/metrics`. |
| `zith.performance.diagnosticsDebounceMs` | integer | 75 | Podem ser expostos em configuração, com clamp entre 25 e 500. |
| `zith.frontend.enabled` | boolean | `true` | Deve ficar ativo para obter análise cross-file e semântica partilhada. |
| `zith.frontend.warmupStdlib` | boolean | `true` | Pré-aquece o stdlib; útil para primeiras respostas mais rápidas. |
| `zith.frontend.maxWorkers` | integer | 0 | `0` deixa o servidor escolher. Não é preciso expor no VS Code. |
| `zith.frontend.statusNotifications` | boolean | `true` | Necessário para mostrar o estado do warmup na barra de estado da extensão. |

Noutros clientes Zith (vim, neovim, helix), estes valores são opcionais. Na
extensão VS Code, os campos de frontend devem estar ativos por predefinição.

### 1.4 Capacidades anunciadas

O `initialize` responde com o seguinte contrato mínimo:

```json
{
  "textDocumentSync": {
    "openClose": true,
    "change": 2,
    "save": { "includeText": false }
  },
  "completionProvider": {
    "triggerCharacters": [".", ":", "->", " ", "/"],
    "resolveProvider": true
  },
  "hoverProvider": true,
  "inlayHintProvider": true,
  "signatureHelpProvider": {
    "triggerCharacters": ["(", ","]
  },
  "definitionProvider": true,
  "referencesProvider": true,
  "documentHighlightProvider": true,
  "documentSymbolProvider": true,
  "workspaceSymbolProvider": true,
  "workspace": {
    "didChangeWatchedFiles": {
      "dynamicRegistration": false
    }
  },
  "semanticTokensProvider": {
    "legend": {
      "tokenTypes": [
        "function", "struct", "type", "variable", "string",
        "number", "comment", "keyword", "modifier"
      ],
      "tokenModifiers": [
        "declaration", "definition", "readonly", "static"
      ]
    },
    "full": { "delta": true }
  },
  "foldingRangeProvider": true,
  "renameProvider": { "prepareProvider": true },
  "documentFormattingProvider": true,
  "codeActionProvider": {
    "codeActionKinds": ["quickfix"]
  },
  "executeCommandProvider": {
    "commands": [
      "zith.new", "zith.build", "zith.check", "zith.run",
      "zith.fmt", "zith.prepareRun", "zith.stop"
    ]
  },
  "experimental": {
    "zith": {
      "workspaceIndexStatus": true,
      "requestSaveAll": true,
      "frontendSharedContext": true,
      "frontendWarmupStatus": true,
      "frontendMetrics": true,
      "crossFileDiagnostics": true
    }
  }
}
```

O `vscode-languageclient` usa estas capacidades para ativar automaticamente
completion, hover, inlay hints, signature help, definição, referências,
destaques, símbolos, semantic tokens, folding, rename, formatação e code
actions. Os comandos `zith.*` precisam, além disso, de comandos VS Code
registados manualmente.

As bandeiras em `capabilities.experimental.zith` anunciam as extensões Zith que
o cliente pode consumir. A extensão VS Code usa hoje apenas `requestSaveAll`
através da capacidade experimental enviada no `initialize`.

## 2. Metodos LSP

### 2.1 Ciclo de vida dos documentos

| Método | Necessário na extensão? | Observação |
|---|---|---|
| `textDocument/didOpen` | Sim | `vscode-languageclient` envia automaticamente ao abrir ficheiro Zith. |
| `textDocument/didChange` | Sim | O servidor aceita mudanças incrementais ou substituição total. |
| `textDocument/didSave` | Sim | O servidor reencaminha diagnóstico e invalida o cache quando necessário. |
| `textDocument/didClose` | Sim | Remove overlays e limpa diagnósticos do documento. |

O servidor tem `textDocumentSync.change: 2`, portanto mudanças incrementais são
suportadas. O `vscode-languageclient` converte edições do editor em mudanças
com `range`. Não é necessário fazer essa conversão na extensão.

### 2.2 Pedidos de análise

Estes pedidos devem funcionar sem configuração adicional quando a extensão usa
o `LanguageClient`:

| Método | Resultado esperado | Ação no VS Code |
|---|---|---|
| `textDocument/completion` | `CompletionItem[]` | Sugestões normais do editor. |
| `completionItem/resolve` | Item com `documentation` | Deve ser ativado porque `resolveProvider: true`. |
| `textDocument/hover` | `Hover` ou `null` | Popover normal do editor. |
| `textDocument/inlayHint` | `InlayHint[]` | Inlay hints, ativados por capacidade. |
| `textDocument/signatureHelp` | `SignatureHelp` ou `null` | Assinatura ao chamar funções. |
| `textDocument/definition` | Localização ou `null` | Go to definition. |
| `textDocument/references` | `Location[]` | Find all references. |
| `textDocument/documentHighlight` | `DocumentHighlight[]` | Destaque das ocorrências. |
| `textDocument/documentSymbol` | `DocumentSymbol[]` | Outline do documento. |
| `workspace/symbol` | `SymbolInformation[]` | Pesquisa de símbolos no workspace. |
| `textDocument/semanticTokens/full` | `SemanticTokens` | Coloração semântica. |
| `textDocument/semanticTokens/full/delta` | `SemanticTokensDelta` ou full | Coloração incremental. |
| `textDocument/prepareRename` | Intervalo ou `null` | Validar rename. |
| `textDocument/rename` | `WorkspaceEdit` | Rename. |
| `textDocument/formatting` | `TextEdit[]` | Formatação do documento. |
| `textDocument/foldingRange` | `FoldingRange[]` | Folding. |
| `textDocument/codeAction` | `CodeAction[]` | Quick fixes. |

### 2.3 Erros e cancelamento

O servidor usa os códigos de erro LSP padrão:

| Código | Significado |
|---:|---|
| `-32002` | Pedido antes de `initialize`. |
| `-32600` | Violação de ciclo de vida. |
| `-32601` | Método ou comando desconhecido. |
| `-32602` | Parâmetros inválidos, documento não aberto. |
| `-32603` | Erro interno do servidor. |
| `-32800` | Pedido cancelado ou substituído por revisão mais recente. |

A extensão deve tratar `-32800` como silencioso e retryable. Não deve mostrar
erro ao utilizador por cancelamento normal. O `vscode-languageclient` já envia
`$/cancelRequest` quando um pedido é cancelado pelo editor.

## 3. Comandos e notificacoes Zith

### 3.1 Comandos do workspace

Os comandos são invocados com `workspace/executeCommand`. A extensão VS Code
deve registar comandos locais `zith.*` que chamam o `LanguageClient` e decidem
quando guardar ficheiros.

| Comando | Argumentos | Resultado |
|---|---|---|
| `zith.new` | `name` string, obrigatório | `{ "success": boolean, "rootUri": string }` |
| `zith.build` | opcional: diretório ou file URI | `{ "success": boolean, "programUri": string, "codegenAvailable": boolean }` |
| `zith.check` | opcional: file URI | `{ "success": boolean }` |
| `zith.run` | opcional: diretório ou file URI | `{ "success": boolean, "taskId": string, "exitCode": int, "programUri": string, "codegenAvailable": boolean }` |
| `zith.fmt` | opcional: file URI | `TextEdit[]` |
| `zith.prepareRun` | opcional: file URI | Descreve lançamento sem linkar nem executar. |
| `zith.stop` | `taskId` string, obrigatório | `{ "stopped": boolean }` |

Comportamento importante:

- `zith.build` e `zith.run` usam ficheiros guardados em disco. A extensão deve
  pedir `workspace.saveAll(false)` antes de os invocar, ou exigir ficheiro
  limpo.
- `zith.check`, `zith.fmt` e `zith.prepareRun` usam o buffer aberto quando
  existir.
- `zith.compile` foi removido e não deve ser enviado.
- `zith.run` não bloqueia. O resultado contém `taskId`. Processo e output são
  notificações.

### 3.2 Notificações do servidor para a IDE

| Notificação | Quando | Uso no VS Code |
|---|---|---|
| `textDocument/publishDiagnostics` | após open/change/save | Problems panel. |
| `window/logMessage` | logs | Output channel ou log. |
| `$/logTrace` | com `$/setTrace` ativo | Depuração. |
| `window/workDoneProgress/create` | antes de comandos longos | Progresso de build/run. |
| `$/progress` | durante comandos longos | Barra de progresso. |
| `client/registerCapability` | inicialização | Registo do watcher `**/*.zithm`. |
| `zith/frontendStatus` | warmup do stdlib | Estado na barra de estado. |
| `zith/codegenStatus` | codegen LLVM desativado | Aviso ao utilizador. |
| `zith/metrics` | com métricas ativadas | Diagnóstico de desempenho. |
| `zith/processOutput` | durante `zith.run` | Output do processo. |
| `zith/processExit` | fim de `zith.run` | Mostrar saída/erro. |

### 3.3 Progresso e processos

Para `zith.build`, `zith.run` e `zith.prepareRun`, o servidor envia
`window/workDoneProgress/create` seguido de `$/progress` com `begin`,
`report` e `end`.

Durante `zith.run`, o servidor envia:

```json
{
  "taskId": "abc123",
  "chunk": "stdout ou stderr"
}
```

E no fim:

```json
{
  "taskId": "abc123",
  "exitCode": 0
}
```

A extensão VS Code pode mapear `taskId` para um `Terminal` ou para um
`OutputChannel` dedicado. Deve oferecer `zith.stop` quando `zith.run` está
ativo.

## 4. Estado atual da extensao

### 4.1 Já implementado

O ficheiro `zith-extension/vs-code/extension.js` (extensão `0.4.0`) já tem:

- inicialização do `LanguageClient` com `transport: 0` (stdio)
- `documentSelector` `{ scheme: 'file', language: 'zith' }` e associação da
  extensão de ficheiro canónica `.zithm` no manifesto
- `outputChannel` `Zith LSP` para o stderr do servidor
- política de reinício após crash (3 reinícios em 60 s) e comando manual de
  reinício que não pára um cliente já parado
- download/instalação do binário `zith-lsp` e do stdlib através de
  `scripts/download-lsp.js`, com atualização forçada e fallback para binário de
  build local
- tratamento de `zith/requestSaveAll`, `zith/processOutput`, `zith/processExit`
  e `zith/codegenStatus`
- comandos VS Code `zith.build`, `zith.check`, `zith.run`, `zith.fmt`,
  `zith.new`, `zith.stop`, `zith.restartLsp`, `zith.downloadLsp` e
  `zith.triggerSuggest`, todos encaminhados por `workspace/executeCommand`
- `zith.prepareRun` registado como comando interno, fora da paleta
- `DocumentFormattingEditProvider` que aplica os `TextEdit[]` de `zith.fmt`
- middleware que remove itens de completion sem tipo para manter os ícones
  específicos de símbolo
- remoção do auto-registo de comandos do `vscode-languageclient`, porque o
  servidor anuncia os mesmos nomes `zith.*` que a extensão regista

Estes pontos são cobertos por `test/integration/test-extension.js` (payload de
inicialização, política de gravação, encaminhamento de comandos, notificações,
aplicação de `TextEdit[]`, aviso de codegen e limpeza do manifesto) e por
`test/unit/test-download.js` e `test/unit/test-project.js`.

### 4.2 Em falta ou desatualizado

A extensão atual não liga ainda, de forma explícita, aos seguintes pontos da
API:

- `initializationOptions.zithm.frontend` com `enabled`, `warmupStdlib`,
  `maxWorkers` e `statusNotifications`: o `startLspClient` envia apenas
  `stdlibPath`, portanto o servidor usa os valores por omissão do frontend
- `zith/frontendStatus` na barra de estado
- `initializationOptions.zithm.performance.metrics` para configuração de
  depuração e consumo de `zith/metrics`
- cancelamento explícito de pedidos obsoletos: o cliente entrega o cancelamento
  ao `vscode-languageclient`, mas a extensão não envia `$/cancelRequest` por
  conta própria nem descarta respostas por `(id, version)`

Os comandos `zith.compile`, `zith.clean` e `zith.repl` foram removidos e não
devem ser reenviados. O `zith.prepareRun` existe como comando interno do
servidor e da extensão, sem entrada na paleta.

## 5. Referência testada

### 5.1 Nomes de comandos

Nomes registados pela extensão no VS Code e enviados por
`workspace/executeCommand`. O `vscode-languageclient` não regista estes nomes
por conta própria.

| Comando VS Code | Argumentos | Resultado do servidor | Paleta |
|---|---|---|---|
| `zith.build` | diretório ou file URI (opcional) | `{ success, programUri, codegenAvailable }` | sim |
| `zith.check` | file URI (opcional) | `{ success }` | sim |
| `zith.fmt` | file URI (opcional) | `TextEdit[]` | sim |
| `zith.run` | diretório ou file URI (opcional) | `{ success, taskId, exitCode, programUri, codegenAvailable }` | sim |
| `zith.new` | `name` (string, obrigatório) | `{ success, rootUri }` | sim |
| `zith.stop` | `taskId` (string, obrigatório) | `{ stopped }` | sim |
| `zith.prepareRun` | file URI (opcional) | descritor de lançamento, sem linkar nem executar | não |
| `zith.restartLsp` | nenhum | comando local, sem pedido ao servidor | sim |
| `zith.downloadLsp` | nenhum | comando local, sem pedido ao servidor | sim |
| `zith.triggerSuggest` | nenhum | comando local que abre o popup de sugestões | sim |

`zith.build` e `zith.run` operam sobre ficheiros guardados em disco, portanto a
extensão chama `workspace.saveAll(false)` antes de os invocar. `zith.check`,
`zith.fmt` e `zith.prepareRun` usam o buffer aberto quando existe.

### 5.2 Configurações da extensão

O `package.json` declara três definições em `zith.*`:

| Definição | Tipo | Predefinição | Consumida por `extension.js` |
|---|---|---:|---|
| `zith.lsp.enabled` | boolean | `true` | sim, para desativar o arranque do servidor |
| `zith.lsp.stdlibPath` | string | `""` | sim, enviada como `initializationOptions.stdlibPath` |
| `zith.enableHighlighting` | boolean | `true` | não, declarada mas sem leitura no código |

O `initializationOptions` que a extensão envia contém apenas `stdlibPath`. Os
campos `zith.performance.*` e `zith.frontend.*` da secção 1 não são enviados
hoje, pelo que o servidor aplica as predefinições.

### 5.3 Evidência de teste

Testes executados nesta revisão:

- `python3 -m unittest discover -s tests -p "test_lsp.py"` em `zith-lsp`:
  86 testes, incluindo o conjunto de comandos `zith.*`, o aviso
  `zith/codegenStatus`, o fluxo `zith.run`/`zith.stop` e `zith/metrics`.
- `npm test` em `zith-extension/vs-code`: testes unitários e de integração, com
  o payload de inicialização, a política de gravação, as notificações
  `zith/processOutput` e `zith/processExit`, a ausência de `zith.compile` e a
  associação de `.zithm`.

O harness de extension host (`node test/integration/run-host.js`) e o teste de
VSIX empacotado requerem um host VS Code capaz de abrir janela. Ver a limitação
em baixo.

### 5.4 Limitações conhecidas

- macOS e Linux musl estão fora do âmbito de suporte da beta. O downloader
  mapeia `darwin` para `zith-lsp-macos-universal`, mas essa rota não é uma
  afirmação de suporte. As plataformas suportadas e os respetivos estados de
  evidência são os da matriz do passo 1 do roadmap de beta.
- O harness de extension host em Linux não completa neste ambiente: o wrapper
  entrega o controlo a uma instância `code-oss` já em execução e expira. As
  capacidades são verificadas ao nível do LSP e por mocks de extensão, não por
  um host VS Code real aqui.
- O caminho de completion parcial `isIncomplete` está adiado e documentado em
  `zith-lsp/docs/completion-recommendation-plan.md`. O executor só serve
  completion depois da análise ter chegado a `TypeChecked`.
- `workspace/symbol` examina os documentos abertos de forma síncrona e usa o
  índice assíncrono do workspace construído no `initialized`. Não tem limite de
  resultados documentado nem fase de warm-up bloqueante.
- A extensão não mostra `zith/frontendStatus` nem `zith/metrics` ao utilizador.
  O único indicador de estado é o item da barra criado durante `zith.run`.
- `zith.enableHighlighting` está declarada mas não é lida, portanto alterná-la
  não altera o comportamento.

## 6. Exemplos de configuracao

### 6.1 Arranque do servidor

```js
const serverPath = context.asAbsolutePath('server/zith-lsp');
const stdlibPath = vscode.workspace.getConfiguration('zith.lsp')
  .get('stdlibPath', '');

const serverOptions = {
  run: { command: serverPath, transport: 0 },
  debug: { command: serverPath, transport: 0 },
};
```

### 6.2 Pedido de build

```js
const result = await lspClient.sendRequest('workspace/executeCommand', {
  command: 'zith.build',
  arguments: [editor.document.uri.toString()],
});

if (result.codegenAvailable === false) {
  vscode.window.showWarningMessage(
    'O build está ligado a código nativo, mas codegen LLVM está desativado nesta build do zith-lsp.'
  );
}
```

### 6.3 Pedido de run

```js
const result = await lspClient.sendRequest('workspace/executeCommand', {
  command: 'zith.run',
  arguments: [editor.document.uri.toString()],
});

if (result.taskId) {
  runningTaskId = result.taskId;
}
```

### 6.4 Parar run

```js
await lspClient.sendRequest('workspace/executeCommand', {
  command: 'zith.stop',
  arguments: [runningTaskId],
});
```

## Referências

- `docs/ide-extension-api.md` no repositório `zith-lsp`: contrato detalhado do
  servidor.
- `docs/lsp-functional-features.md`: funcionalidades atuais e planeadas.
- `docs/lsp-architecture.md`: arquitetura interna do servidor.
- `zith-extension/vs-code/extension.js`: implementação atual da extensão.
