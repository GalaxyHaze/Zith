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
4. [Estado atual da extensão](#4-estado-atual-da-extensao): o que já existe no
   VS Code e o que falta.
5. [Lista de implementação](#5-lista-de-implementacao): passos concretos para
   a próxima versão da extensão.
6. [Exemplos de configuração](#6-exemplos-de-configuracao): snippets JSON-RPC.

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
  }
}
```

O `vscode-languageclient` usa estas capacidades para ativar automaticamente
completion, hover, inlay hints, signature help, definição, referências,
destaques, símbolos, semantic tokens, folding, rename, formatação e code
actions. Os comandos `zith.*` precisam, além disso, de comandos VS Code
registados manualmente.

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

O ficheiro `zith-extension/vs-code/extension.js` já tem:

- inicialização do `LanguageClient` com `transport: 0`
- `documentSelector` para `zith`
- `outputChannel` `Zith LSP`
- política de reinício após crash (3 reinícios em 60 s)
- download/instalação do binário `zith-lsp` e do stdlib
- fallback para binário de build local
- tratamento de `zith/requestSaveAll`
- comandos VS Code `zith.build`, `zith.check`, `zith.run`, `zith.clean`,
  `zith.fmt`, `zith.repl`, `zith.restartLsp` e `zith.downloadLsp`;
- validação de ficheiro ativo e projeto `ZithProject.toml`.

### 4.2 Em falta ou desatualizado

A extensão atual não liga ainda, de forma explícita, aos seguintes pontos da
API:

- `initializationOptions.zithm.frontend` com `enabled`, `warmupStdlib`,
  `maxWorkers` e `statusNotifications`
- `zith/frontendStatus` na barra de estado
- `zith/codegenStatus` com aviso `codegenAvailable: false`
- notificações `zith/processOutput` e `zith/processExit` para `zith.run`
- progresso `window/workDoneProgress/create` e `$/progress`
- `zith.prepareRun` e `zith.stop` através de comandos VS Code
- cancelamento explícito de pedidos obsoletos
- `initializationOptions.zithm.performance.metrics` para configuração de
  depuração
- remoção do comando `zith.compile`, uma vez que o servidor já não o anuncia
- `codegenAvailable` nas respostas de `zith.build` e `zith.run`

## 5. Lista de implementacao

### 5.1 Inicialização

1. Adicionar no `package.json` configurações para:

```json
{
  "zith.lsp.diagnosticsDebounceMs": 75,
  "zith.lsp.warmupStdlib": true,
  "zith.lsp.metrics": false
}
```

2. Construir `initializationOptions` no `startLspClient`:

```js
const metrics = config.get('metrics', false);
const debounce = config.get('diagnosticsDebounceMs', 75);
const warmupStdlib = config.get('warmupStdlib', true);

const clientOptions = {
  documentSelector: [{ scheme: 'file', language: 'zith' }],
  outputChannel: getLspOutputChannel(),
  initializationOptions: {
    stdlibPath,
    zith: {
      performance: {
        metrics,
        diagnosticsDebounceMs: debounce,
      },
      frontend: {
        enabled: true,
        warmupStdlib,
        maxWorkers: 0,
        statusNotifications: true,
      },
    },
  },
};
```

3. Manter o envio da capacidade experimental para `fillInitializeParams`, se
   ainda for necessário para o protocolo atual.

### 5.2 Barra de estado e notificações

1. Criar um `StatusBarItem` para Zith.
2. Ouvir `zith/frontendStatus`:

```js
client.onNotification('zith/frontendStatus', params => {
  if (params.state === 'warming') {
    statusBar.text = '$(sync~spin) Zith: warming';
    statusBar.tooltip = 'Pré-aquecer stdlib';
  } else if (params.state === 'ready') {
    statusBar.text = '$(check) Zith';
    statusBar.tooltip = 'Language server pronto';
  } else if (params.state === 'error') {
    statusBar.text = '$(error) Zith';
    statusBar.tooltip = params.message || 'Erro no frontend';
  }
  statusBar.show();
});
```

3. Ouvir `zith/codegenStatus` e mostrar aviso quando
   `codegenAvailable === false`.
4. Ouvir `zith/metrics` quando `metrics` estiver ativa e enviar para o output
   channel.

### 5.3 Build, check e run

1. Preservar comandos de `zith.build`, `zith.check` e `zith.run` que chamam
   `workspace/executeCommand`.
2. Usar `workspace.saveAll(false)` antes de `zith.build` e `zith.run`, com
   confirmação opcional.
3. Ler `codegenAvailable` nas respostas e mostrar aviso quando for `false`.
4. Registar `zith.prepareRun` para gerar uma configuração de launch sem
   executar.
5. Registar `zith.stop` para terminar a tarefa associada ao `taskId`.
6. Não registar, nem anunciar, `zith.compile`.

### 5.4 Processos e progresso

1. Criar um helper `getRunChannel(taskId)` que cria/cacheia um
   `OutputChannel`.
2. Ouvir `zith/processOutput`:

```js
client.onNotification('zith/processOutput', params => {
  const channel = getRunChannel(params.taskId);
  channel.append(params.chunk);
});
```

3. Ouvir `zith/processExit`:

```js
client.onNotification('zith/processExit', params => {
  const channel = getRunChannel(params.taskId);
  channel.appendLine(`\n[exit ${params.exitCode}]`);
  // terminar estado de execução, se houver
});
```

4. Usar o progresso do `vscode-languageclient` ou tratar
   `window/workDoneProgress/create` e `$/progress` manualmente.

### 5.5 Cancelamento e estado

1. Guardar o `version` de cada documento quando relevante.
2. Cancelar pedidos antigos com `client.sendRequest('$/cancelRequest', { id })`
   apenas quando o cliente do VS Code não os gerir sozinho.
3. Tratar `-32800` como silencioso.
4. Quando o servidor reiniciar, limpar canais de run, estado da barra e
   informação de `taskId`.

### 5.6 Testes

1. Estender `test/integration/test-extension.js` com um mock que responda a
   `initialize` com as capacidades completas.
2. Testar `zith/frontendStatus` e `zith/processOutput` no mock.
3. Testar resposta `zith.build` sem `success` e com `codegenAvailable: false`.
4. Testar que `zith.compile` já não é anunciado.
5. Testar que `zith.stop` recebe `taskId` do `zith.run`.

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
