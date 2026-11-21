# Plano: Atributos `#[...]` MVP no Zith--

## Objetivo
Adicionar ao compilador Zith-- o parsing e o semantic MVP de atributos `#[a, b]` e `#[a] #[b]`, com `discardable` em funções e `volatile` em variáveis locais/globais, persistindo `discardable` no ZIRL/cache, e atualizando docs, formatter, reconstruct e testes.

## Pré-condições
- `cwd` é `/home/diogo/Zith`.
- O build já está configurado em `/home/diogo/Zith/build`.
- A falha pré-existente `test-hir-lower-modern` sobre `opaque` cross-module pode continuar em vermelho; não é causada por este trabalho.
- Não correr `cmake --build build --target fmt`; reformata ficheiros não relacionados.
- Não usar `git reset --hard` nem reverter alterações uncommitted feitas pelo utilizador.
- Não alterar ficheiros fora dos listados neste plano.
- A skill de execução é Sequenta apenas se o utilizador aprovar explicitamente antes do trabalho começar. Sem aprovação, executar de forma linear normal.

## Ficheiros que serão alterados
- `/home/diogo/Zith/src/frontend/frontend.hpp`
- `/home/diogo/Zith/src/frontend/ast-lowerer.cpp`
- `/home/diogo/Zith/src/frontend/ast-lowerer.hpp`
- `/home/diogo/Zith/src/frontend/frontend-decl.cpp`
- `/home/diogo/Zith/src/frontend/frontend-stmt.cpp`
- `/home/diogo/Zith/src/session/frontend-module-analysis.cpp`
- `/home/diogo/Zith/src/session/frontend-context.hpp`
- `/home/diogo/Zith/src/session/frontend-symbol-resolution.cpp`
- `/home/diogo/Zith/src/symbols/symbol-table.hpp`
- `/home/diogo/Zith/src/symbols/symbol-table.cpp`
- `/home/diogo/Zith/src/frontend/frontend-printer.cpp`
- `/home/diogo/Zith/src/frontend/frontend.cpp`
- `/home/diogo/Zith/src/sema/sema-modern.hpp`
- `/home/diogo/Zith/src/sema/sema-control.cpp`
- `/home/diogo/Zith/src/sema/sra-facts.cpp`
- `/home/diogo/Zith/src/sema/hir-lower-modern.cpp`
- `/home/diogo/Zith/src/hir/hir-module.hpp`
- `/home/diogo/Zith/src/hir/hir-attrs.hpp`
- `/home/diogo/Zith/src/codegen/codegen-emit-stmt.cpp`
- `/home/diogo/Zith/src/codegen/codegen-emit-expr.cpp`
- `/home/diogo/Zith/src/sema/hir-lower-stmt.cpp`
- `/home/diogo/Zith/src/cache/cache-types.hpp`
- `/home/diogo/Zith/src/cache/artifact-builder.cpp`
- `/home/diogo/Zith/src/cache/artifact-builder.hpp`
- `/home/diogo/Zith/src/zirl/zirl-header.hpp`
- `/home/diogo/Zith/src/zirl/zirl-decl-section.cpp`
- `/home/diogo/Zith/src/zirl/zirl-decl-section.hpp`
- `/home/diogo/Zith/src/zirl/zirl-code-section.cpp`
- `/home/diogo/Zith/src/zirl/zirl-code-section.hpp`
- `/home/diogo/Zith/src/zirl/zirl-attrs-section.cpp`
- `/home/diogo/Zith/src/zirl/zirl-attrs-section.hpp`
- `/home/diogo/Zith/src/session/persistent-cache.cpp`
- `/home/diogo/Zith/src/formatter/fmt-visitor.cpp`
- `/home/diogo/Zith/tests/test-frontend.cpp`
- `/home/diogo/Zith/tests/test-frontend-modern-pipeline.cpp`
- `/home/diogo/Zith/tests/test-cache.cpp`
- `/home/diogo/Zith/tests/test-zirl-sections.cpp`
- `/home/diogo/Zith/tests/test-formatter.cpp`
- `/home/diogo/Zith/docs/Zith--.md`
- `/home/diogo/Zith/docs/Zith---implementation.md`
- `/home/diogo/Zith/docs/agents/memory.md` (apenas se este ficheiro existir; caso contrário ignorar)

## Decisões já fechadas (não reabrir)
- Lexar `#` como punctuação normal e parsear grupos `#[...]`.
- Aceitar `#[a, b]` e `#[a] #[b]`; `a, b` são nomes simples de atributo, sem valores.
- Atributos vêm antes de visibilidade e antes da declaração/binding.
- Atributo desconhecido gera warning, não erro.
- Atributo conhecido aplicado ao alvo errado gera warning com código separado, não erro.
- Semântica: `discardable` só em funções; `volatile` só em variáveis locais e globais.
- `discardable` permite chamada não-void como `Expression` sem `_ = call();`.
- `discardable` cobre `Call` e `DockCall`, incluindo methods/overloads e módulos importados.
- `volatile` no MVP só marca o slot direto para LLVM.
- `volatile` não acumula factos SRA e não participa em narrowing; o utilizador deve copiar o valor ou usar `raw`.
- `assume`, `ensure`, `maybe` e afins ficam para Zith completo, fora do Zith--.
- Formatter e `reconstruct()` preservam texto original dos atributos e a ordem.
- Cache/ZIRL faz version bump e `discardable` entra no public ABI hash.
- `HirFnAttrs` ganha `discardable`; `volatile` não viaja cross-module nesta fase.

## Ordem de execução

### Step 1 - Lexar `#`

**Goal:** O lexer em `/home/diogo/Zith/src/frontend/ast-lowerer.cpp` classifica `#` como `TokenKind::Punctuation` sem diagnosticar `unknown token`.

**Sub-steps:**
1. Abrir `/home/diogo/Zith/src/frontend/ast-lowerer.cpp`.
2. Localizar a função anónima `isPunctuation(char character)` na linha 41.
3. Trocar a string `"()[]{}:;,.@\`"` por `"()[]{}:;,.@\`#"`.
4. Não mudar o lexing de `Dots`, de operadores ou de literais.

**Comando:**
```
./build/tests/test-frontend || true
```
O teste que falhar nesta fase é esperado se ainda não existir para atributos; isto é apenas uma smoke check de que o build não parte antes de editar os testes.

**Expected output:** Não deve aparecer `unknown token`.

**Failure checks:**
- Se `isPunctuation` tem outra constante em outro ficheiro, executar `rg -n "isPunctuation" /home/diogo/Zith/src` e atualizar apenas a função do lexer.
- Se aparecer `unknown token` num ficheiro de exemplo que use `#`, verificar que a string foi trocada.

**Success criteria:** `rg -n '"\(\)\[\]\{\}:;,\.@`#' /home/diogo/Zith/src/frontend/ast-lowerer.cpp` encontra a nova string.

### Step 2 - Adicionar o modelo AST de atributos

**Goal:** `Declaration`, `Binding` e `Statement` carregam uma lista ordenada de atributos com `kind`, `text` e `span`.

**Sub-steps:**
1. Abrir `/home/diogo/Zith/src/frontend/frontend.hpp`.
2. Inserir após o enum `BindingKind` (perto da linha 115):
```cpp
enum class AttributeKind : uint8_t { Unknown, Discardable, Volatile };

struct Attribute {
    AttributeKind kind = AttributeKind::Unknown;
    std::string text;
    TextSpan span;
};
```
3. Adicionar `std::vector<Attribute> attributes;` a `struct Declaration`.
4. Adicionar `std::vector<Attribute> attributes;` a `struct Binding`.
5. Adicionar `std::vector<Attribute> attributes;` a `struct Statement`.
6. Não reutilizar `Expression::attributes`, que pertence a `MacroCall`.
7. Atualizar qualquer `std::memcpy`/agregado que copie `Declaration` ou `Binding` se o compilador reclamar de construtora copiada; procurar por `Declaration{` e `Binding{` no repositório com `rg -n "Declaration\\{|Binding\\{" /home/diogo/Zith/src`.

**Comando:**
```
cmake --build build -j4 --target zithc 2>&1 | tail -80
```

**Expected output:** Build termina sem erros C++.

**Failure checks:**
- Se o compilador reclama de `no matching constructor`, ajustar apenas os inicializadores agregados encontrados pela `rg` acima, acrescentando `{}` para a nova lista ou movendo a lista para o fim do struct.
- Se `std::memcpy` é usado sobre `Declaration`, mover a nova lista para depois dos campos copiados ou substituir a cópia por operações por campo.

**Success criteria:** `cmake --build build -j4 --target zithc` sai com código 0 e `rg -n "AttributeKind" /home/diogo/Zith/src/frontend/frontend.hpp` encontra o enum.

### Step 3 - Parser de atributos no top level

**Goal:** `AstLowerer::run()` consome zero ou mais grupos `#[...]` antes de visibilidade e declarações, e as declarações guardam a lista.

**Sub-steps:**
1. Abrir `/home/diogo/Zith/src/frontend/ast-lowerer.hpp`.
2. Declarar na secção privada de `AstLowerer`, junto às funções de lower:
```cpp
[[nodiscard]] std::vector<Attribute> parseAttributes();
```
3. Abrir `/home/diogo/Zith/src/frontend/ast-lowerer.cpp`.
4. Implementar `AstLowerer::parseAttributes()` antes de `AstLowerer::run()`:
   - Se `index_ >= token_count_` ou `snapshot_.tokens_[index_].kind != TokenKind::Punctuation` ou `text(index_) != "#"`, devolver lista vazia.
   - Enquanto `index_ < token_count_` e o token for `#`:
     - Consumir `#`.
     - Se não houver `[`, reportar `{tokenSpan(index_ - 1U), "expected '[' after '#'"}` e devolver o acumulado.
     - Consumir `[`.
     - Enquanto não houver `]`:
       - Se houver `,`, consumir e continuar.
       - Se o token for `Identifier` ou `Keyword`, criar `Attribute{AttributeKind::Unknown, std::string(text(index_)), tokenSpan(index_)}`, avançar e continuar.
       - Caso contrário, reportar `{tokenSpan(index_), "expected attribute name"}` e avançar até `]` ou `,` para não travar.
     - Consumir `]`.
   - Não aceitar valores com `=`.
5. Na `AstLowerer::run()`, antes do `while (index_ < token_count_)`, adicionar:
```cpp
std::vector<Attribute> pending_attributes;
```
6. No início de cada iteração do `while`, após `const uint32_t start = index_;`, chamar:
```cpp
if (pending_attributes.empty())
    pending_attributes = parseAttributes();
if (!pending_attributes.empty()) {
    flushBadRun();
}
```
7. Em cada branch que cria declaração no `run()` (`lowerImport`, `lowerMacroDeclaration`, `lowerDeclaration`, `lowerImplementBlock`), mover `pending_attributes` para a chamada correspondente e depois limpar `pending_attributes.clear()`.
   - Para `extern`, `use`, `unsafe`, `;`, `global`, `flow fn` e `marker`, se o atributo não for aplicável, limpar `pending_attributes` sem passar.
8. Em `/home/diogo/Zith/src/frontend/frontend-decl.cpp`, alterar as assinaturas para aceitar `const std::vector<Attribute> &attributes = {}` como último parâmetro:
   - `lowerImport`.
   - `lowerMacroDeclaration`.
   - `lowerImplementBlock` (a lista é para o bloco, mas as listas dos métodos são lidas no membro).
   - `lowerDeclaration`.
9. Dentro das implementações, copiar `declaration.attributes = attributes;` para `Declaration` após `declaration.id`.
10. Em `lowerImplementBlock`, no loop de membros, antes de `isVisibilityPrefix()`, declarar `const auto method_attributes = parseAttributes();` e passá-lo à `lowerDeclaration`.
11. Em `lowerDeclaration`, nos loops de membros de `Struct`, `Interface`, `Enum`, `Union` e `Trait`, antes de `functionKindPrefix()`, declarar `const auto method_attributes = parseAttributes();` e passá-lo à chamada de `lowerDeclaration`.

**Comando:**
```
cmake --build build -j4 --target zithc 2>&1 | tail -80
```

**Expected output:** Build termina e `#[discardable] fn println(): i32;` é aceite sem diagnostic.

**Failure checks:**
- Se `isPunctuation` não reconhece `[`/`]` no parser, usar a função existente `punctuation(index_, '[')`.
- Se `TokenKind::Punctuation` com texto `#` não é atual, confirmar com `sed -n '215,240p' /home/diogo/Zith/src/frontend/ast-lowerer.cpp` que o lexer produz `Punctuation`.
- Se `parseAttributes` já existe em `/home/diogo/Zith/src/frontend/frontend-expr-primary.cpp` com outro propósito, nomear a nova função `parseDeclAttributes` para evitar conflito.

**Success criteria:** `./build/zithc --include stdlib check /tmp/attr-smoke.zithm` com conteúdo `#[discardable] fn println(): i32;` não mostra erros de parsing e `rg -n "parseAttributes" /home/diogo/Zith/src/frontend/ast-lowerer.hpp` encontra a declaração.

### Step 4 - Parser de atributos em statements locais

**Goal:** Bindings locais `let`/`var`/`const` aceitam `#[volatile]` antes do keyword.

**Sub-steps:**
1. Abrir `/home/diogo/Zith/src/frontend/frontend-stmt.cpp` e localizar `AstLowerer::parseStatements()` e `AstLowerer::parseFor()`.
2. Em `parseStatements()`, no início do loop de statements, antes de ler a palavra-chave do statement, chamar:
```cpp
std::vector<Attribute> statement_attrs = parseAttributes();
```
3. No caso `let`/`var`/`const`, copiar `statement_attrs` para `statement.attributes` e para `statement.binding.attributes`.
4. No destructuring (`let [x, y] = ...`), copiar `statement_attrs` para `pack_stmt.binding.attributes` e para cada `element_stmt.binding.attributes`.
5. No loop variable de `for` (`for (x in ...)`), chamar `parseAttributes()` antes de ler o nome e copiar para o binding sintético.
6. Para statements não-binding com atributos presentes, mover os atributos para `statement.attributes` sem error.

**Comando:**
```
cmake --build build -j4 --target zithc 2>&1 | tail -80
```

**Expected output:** Build termina e `#[volatile] let x: i32 = 0;` dentro de `fn main()` produz um `StmtKind::Binding` com `binding.attributes.size() == 1`.

**Failure checks:**
- Se `statement_attrs` interfere com o parsing de loop labels, verificar que `parseAttributes()` só consome tokens `#[`.
- Se `parseAttributes` não está acessível deste TU, confirmar que está no cabeçalho `ast-lowerer.hpp`.

**Success criteria:** Um teste manual via `frontend::parse` (na fase de testes) vê `binding.attributes[0].kind == frontend::AttributeKind::Volatile`.

### Step 5 - Resolução e transporte de atributos para símbolos

**Goal:** `LocalSymbolInfo`, `ResolvedName` e `SymbolData` transportam `discardable` para sema, cache, codegen e ABI hash.

**Sub-steps:**
1. Abrir `/home/diogo/Zith/src/frontend/frontend.hpp`.
2. Adicionar a `struct Declaration` um helper:
```cpp
[[nodiscard]] bool discardable() const noexcept {
    for (const auto &attr : attributes)
        if (attr.kind == AttributeKind::Discardable)
            return true;
    return false;
}
```
   O tipo `Attribute` precisa de estar declarado antes de `Declaration` no mesmo ficheiro, junto a `BindingKind` (já é o plano do Step 2).
3. Abrir `/home/diogo/Zith/src/session/frontend-context.hpp`.
4. Adicionar `bool discardable = false;` a `struct LocalSymbolInfo` e a `struct ResolvedName`, ambos no fim das respetivas structs para não reordenar inicializadores.
5. Abrir `/home/diogo/Zith/src/session/frontend-module-analysis.cpp`.
6. Na função `FrontendContext::buildModule`, para `LocalSymbolInfo symbol{...}` na linha 391, adicionar `declaration.discardable()` como último campo do inicializador agregado.
7. Esse inicializador passa a ter 11 campos por ordem:
   `id`, `name`, `visibility`, `kind`, `span`, `signature`, `isExtern`, `externalSymbol`, `isVariadic`, `isVariadicSlice`, `discardable`.
8. Abrir `/home/diogo/Zith/src/session/frontend-symbol-resolution.cpp`.
9. Nas criações de `ResolvedName` para funções Zith top-level (linha 241), local `state` (linha 296) e imports (linhas 427 e 465), copiar `declaration.discardable()` ou `symbol.discardable`.
10. Nas imports por namespace (linhas 465 a 475) e métodos de owner (linhas 571 a 580 e 692 a 701), copiar `symbol.discardable`.
11. Não copiar `discardable` para cinterop `Foreign` (fica `false`).
12. Abrir `/home/diogo/Zith/src/symbols/symbol-table.hpp`.
13. Adicionar `bool discardable = false;` a `struct SymbolData`, após `members`.
14. Adicionar parâmetro final `bool discardable = false` a todas as quatro overloads de `SymbolTable::declare` e `declareInScope`.
15. Abrir `/home/diogo/Zith/src/symbols/symbol-table.cpp`.
16. Nas implementações, passar `discardable` ao novo campo do `SymbolData` e reencaminhá-lo nos wrappers `std::string_view`.
17. Em `SymbolTable::emplace` e `SymbolTable::emplace(const SymbolTable &, ScopeId)`, copiar `data.discardable` para a declaração nova.

**Comando:**
```
cmake --build build -j4 --target zithc 2>&1 | tail -80
```

**Expected output:** Build passa e `rg -n "discardable" /home/diogo/Zith/src/session /home/diogo/Zith/src/symbols /home/diogo/Zith/src/frontend/frontend.hpp` encontra os seis pontos acima.

**Failure checks:**
- Se `std::is_aggregate` sobre `ResolvedName` parte, reordenar os campos adicionais para o fim é suficiente; se ainda falhar, procurar inicializadores nomeados com `rg -n "\.discardable|ResolvedName .*\\{" /home/diogo/Zith/src`.
- Se `SymbolData` tem mais construtores no repositório, procurar com `rg -n "SymbolData\\{" /home/diogo/Zith/src` e atualizar todos.
- Se `Declaration::discardable()` não consegue nomear `AttributeKind`, confirmar que o tipo `Attribute` está antes na ordem de declaração do ficheiro.

**Success criteria:** Build C++ passa e `rg` confirma `discardable` em `LocalSymbolInfo`, `ResolvedName`, `SymbolData` e nas quatro declarações de `SymbolTable`.

### Step 5b - Propagar `discardable` dos frontends/cache para `SymbolData`

**Goal:** Ao materializar símbolos e ao hidratar cache, `SymbolData::discardable` fica correto para o ABI hash e para o artifact.

**Sub-steps:**
1. Abrir `/home/diogo/Zith/src/session/compilation-session.cpp`.
2. Em `materializeFrontendSymbols()`, linha 259, adicionar `decl.discardable()` como último argumento da chamada `mSyms.declare`.
3. Confirmar que a chamada muda para:
```cpp
mSyms.declare(decl.name, mapFrontendVisibility(decl.visibility), 0,
              mapFrontendDeclKind(decl.kind), ast::kInvalidDecl, {}, {}, {},
              decl.discardable());
```
4. Abrir `/home/diogo/Zith/src/session/persistent-cache.cpp`.
5. Em `hydrateFromArtifact`, na declaração `mSyms.declare(decl.name, ...)` da linha 140, adicionar `decl.discardable` como último argumento.
6. A chamada passa a:
```cpp
decl_sym_ids.push_back(mSyms.declare(decl.name, decl.visibility, decl.mod_depth,
                                     static_cast<symbols::SymKind>(decl.kind),
                                     ast::kInvalidDecl, {}, {}, {}, decl.discardable));
```
7. Não alterar nenhum outro call site de `SymbolTable::declare` se já usa o default `false`.

**Comando:**
```
cmake --build build -j4 --target zithc 2>&1 | tail -80
```

**Expected output:** Build passa e as duas chamadas de `declare` acima incluem o argumento `discardable`.

**Failure checks:**
- Se a assinatura com default ainda não foi aplicada, o compilador reclama com `no matching function`; aplicar primeiro o Step 5.
- Se outro call site usa 8 argumentos e o default não é suficiente, ajustar apenas os dois pontos pedidos.

**Success criteria:** `rg -n "discardable" /home/diogo/Zith/src/session/compilation-session.cpp /home/diogo/Zith/src/session/persistent-cache.cpp` encontra dois call sites com `discardable`.

### Step 5c - Preencher `ResolvedName` na hidratação de módulos importados

**Goal:** Calls a funções importadas de um módulo cache-hit continuam a ver `discardable` na sema.

**Sub-steps:**
1. Confirmar que `buildResolutions` já copia `symbol.discardable` para `ResolvedName` nas importações (Step 5).
2. A hidratação de um módulo importado mantém `LocalSymbolInfo` sem `discardable` quando o cache hit não repopula `publicSymbols`.
3. Localizar no `CompilationSession` ou `FrontendContext` o ponto onde módulos hidratados são convertidos em `ModuleArtifactPtr` para a resolução.
4. Se esse ponto existe, adicionar o preenchimento de `LocalSymbolInfo::discardable` a partir de `art.decls[i].discardable`, indexando por `decl.name` e `decl.kind`.
5. Se o pipeline atual percorre o fonte antes do cache para importações, não é necessário este passo; deixar como está e seguir para Step 9, onde `art.decls.discardable` é o transporte canónico.

**Comando:**
```
rg -n "ModuleArtifactPtr|hydrateFromArtifact|publicSymbols" /home/diogo/Zith/src/session/persistent-cache.cpp /home/dioogo/Zith/src/session/frontend-context.cpp /home/diogo/Zith/src/session/compilation-session.cpp
```

**Expected output:** `rg` mostra onde `ModuleArtifactPtr` é construído após `hydrateFromArtifact`.

**Failure checks:**
- Se não existe ponto para alterar porque o módulo hidratado nunca entra no `ModuleResolution`, reportar isso no plano com uma nota, mas não bloquear a implementação; o caminho de cache continua a ser validado pelo teste do Step 13.

**Success criteria:** `rg` documentado no plano ou o código fonte fica com a cópia explícita.

### Step 6 - Classificar atributos por alvo

**Goal:** Depois de parsear, `AttributeKind` fica conhecido e atributos inválidos produzem warnings com dois códigos distintos. Este passo já está bem definido no plano atual; apenas corrigir o helper para a API real de `Diagnostic`.

**Sub-steps:**
1. Abrir `/home/diogo/Zith/src/diagnostics/error-codes.hpp`.
2. Adicionar após `DiscardedResult = 2026`:
```cpp
inline constexpr ErrCode const UnknownAttribute          = 2028;
inline constexpr ErrCode const AttributeNotApplicable    = 2029;
```
3. Criar em `/home/diogo/Zith/src/frontend/frontend.hpp` um helper por ficheiro-fonte, não global:
```cpp
inline AttributeKind applyAttributeTarget(Attribute attr, frontend::AttributeTarget target,
                                          std::vector<Diagnostic> &diagnostics) {
    AttributeKind classified = AttributeKind::Unknown;
    if (attr.text == "discardable" && target == AttributeTarget::Function)
        classified = AttributeKind::Discardable;
    if (attr.text == "volatile" && target == AttributeTarget::Variable)
        classified = AttributeKind::Volatile;
    if (classified == AttributeKind::Unknown && attr.text != "discardable" &&
        attr.text != "volatile") {
        diagnostics.push_back({attr.span, "unknown attribute '" + attr.text + "'", true,
                               diagnostics::err::UnknownAttribute});
    } else if (classified == AttributeKind::Unknown) {
        diagnostics.push_back({attr.span, "attribute '" + attr.text + "' is not applicable here",
                               true, diagnostics::err::AttributeNotApplicable});
    }
    return classified;
}
```
4. Em cada ponto de consolidação, gravar `attr.kind = applyAttributeTarget(attr, target, diagnostics)`.
5. Não alterar `Attribute::kind` a partir da própria struct; a classificação acontece no baixador que conhece o alvo.

**Comando:**
```
cmake --build build -j4 --target zithc 2>&1 | tail -80
```

**Expected output:** Build termina e `#[unknown] fn f() {}` produz warning com `UnknownAttribute`, `#[discardable] let x = 1;` produz warning com `AttributeNotApplicable`.

**Failure checks:**
- Se o campo de warning do `Diagnostic` tem outro nome, usar `diagnostics::Severity` e `uint32_t code` tal como faz `frontend-module-analysis.cpp` para mapear warnings.

**Success criteria:** `rg -n "UnknownAttribute" /home/diogo/Zith/src` encontra a constante e a implementação.

### Step 7 - Sema de `discardable` em expression statements

**Goal:** `checkExpressionStatement` omite `DiscardedResult` para calls cujo callee tem `discardable`.

**Sub-steps:**
1. Abrir `/home/diogo/Zith/src/sema/sema-control.cpp`.
2. Em `checkExpressionStatement`, depois de saber `expr.kind == Call || expr.kind == DockCall`, chamar:
```cpp
if (calleeIsDiscardable(stmt.expression))
    return;
```
3. Implementar helper privado em `/home/diogo/Zith/src/sema/sema-modern.hpp` e `.cpp`:
```cpp
bool PerModuleSema::calleeIsDiscardable(frontend::ExprId call_id) const noexcept;
```
4. Em `calleeIsDiscardable`, obter o callee:
   - Para `ExprKind::Call`, usar o primeiro operando da expression.
   - Para `ExprKind::DockCall`, usar o primeiro operando da expression.
5. Usar `findResolvedExpr(callee_id)`.
6. Se `resolved != nullptr && resolved->discardable`, retornar `true`.
7. Se `resolved == nullptr` ou a call é estrangeira, retornar `false`.
8. Repetir o mesmo para `ExprKind::Name` dentro de `calleeIsDiscardable` só se `callee.kind == ExprKind::Name`; para method/field, `findResolvedExpr(callee_id)` pode já ter sido resolvido, mas se for nullptr devolver false.
9. Não mudar `StmtKind::Discard`, assign, binding initializer, return ou outros usos.

**Comando:**
```
cmake --build build -j4 --target test-sema 2>&1 | tail -80
```

**Expected output:** Build do alvo de sema passa.

**Failure checks:**
- Se `findResolvedExpr` não é verificada de `expr.value - 1U`, seguir exatamente o padrão usado em `sema-modern-utils.cpp` e `hir-lower-modern.cpp`.
- Se `ResolvedName::discardable` ainda não está em `sema`, confirmar que o Step 5 foi concluído.

**Success criteria:** Um teste de pipeline com `#[discardable] fn make(): i32` seguido de `make();` não reporta `DiscardedResult`.

### Step 8 - Meter `discardable` no HIR

**Goal:** `HirFunction` e `HirFnAttrs` expõem `discardable`.

**Sub-steps:**
1. Abrir `/home/diogo/Zith/src/hir/hir-module.hpp`.
2. Adicionar `bool discardable = false;` a `struct HirFunction`.
3. Abrir `/home/diogo/Zith/src/hir/hir-attrs.hpp`.
4. Adicionar `bool discardable = false;` a `struct HirFnAttrs`.
5. Alterar `hasResidualFacts()` de `HirFnAttrs` para incluir `|| discardable`.
6. Abrir `/home/diogo/Zith/src/sema/hir-lower-modern.cpp` e localizar a linha 375 onde `attrs.noAlias = true` é preenchido dentro de `lowerFunctionBody`.
7. Nesse mesmo bloco, após o preenchimento já existente de `HirFnAttrs`, adicionar:
```cpp
attrs.discardable = false;
for (const auto &attribute : info.decl->attributes)
    if (attribute.kind == frontend::AttributeKind::Discardable)
        attrs.discardable = true;
```
   A variável `attrs` já existe naquele bloco; fundir com o código existente sem declarar outra.
8. Em `/home/diogo/Zith/src/sema/hir-lower-modern.cpp`, no loop onde `HirFunction` é criado para declarações Zith, manter `function.discardable` a false ou copiar de `info.decl->discardable()` conforme preferência, mas a fonte fiel para cache e sema é `HirFnAttrs`.
9. Não marcar `isForeignC` como discardable.

**Comando:**
```
cmake --build build -j4 --target zithc 2>&1 | tail -80
```

**Expected output:** Build passa e `hir_fn.discardable` aparece no dump HIR quando `--emit-hir` for usado.

**Failure checks:**
- Se `info.decl` pode ser `nullptr` em `lowerFunctionBody`, proteger antes de iterar `attributes`.

**Success criteria:** `rg -n "discardable" /home/diogo/Zith/src/hir /home/diogo/Zith/src/sema/hir-lower-modern.cpp` encontra os novos campos.

### Step 9 - Persistir `discardable` no cache/ZIRL

**Goal:** `discardable` sobrevive a hit de cache e muda o public ABI hash, usando `SymbolData::discardable` sem alterar o construtor de `ArtifactBuilder`.

**Sub-steps:**
1. Abrir `/home/diogo/Zith/src/cache/cache-types.hpp`.
2. Adicionar `bool discardable = false;` a `struct DeclRecord`.
3. Adicionar `bool discardable = false;` a `struct CompactFunction`.
4. Adicionar `bool discardable = false;` a `struct HirFnAttrsRecord`.
5. Abrir `/home/diogo/Zith/src/cache/artifact-builder.cpp`.
6. Em `computePublicAbiHash`, dentro do loop de `syms_`, após escrever `name`, `kind`, `visibility` e `mod_depth`, adicionar também a flag ao `primary` e ao `secondary`:
```cpp
primary << (sym.discardable ? "1" : "0") << '\n';
secondary << (sym.discardable ? "1" : "0") << '\n';
```
7. No loop de `DeclRecord`, após `decl.is_extern`, adicionar `decl.discardable = sym.discardable;`.
8. No loop de `CompactFunction`, adicionar `cfn.discardable = fn.discardable;` junto às flags já copiadas de `HirFunction`.
9. No loop de `HirFnAttrsRecord`, adicionar `rec.discardable = attrs->discardable;`.
10. Não alterar `/home/diogo/Zith/src/cache/artifact-builder.hpp` nem o call site em `/home/diogo/Zith/src/session/persistent-cache.cpp` que constrói `ArtifactBuilder`.
11. Abrir `/home/diogo/Zith/src/zirl/zirl-decl-section.cpp`.
12. No `encodeDecls`, trocar `w.writeU8(0); // reserved` por `w.writeU8(d.discardable ? 1 : 0);`.
13. No `decodeDecls`, ler `reserved` numa variável e fazer `d.discardable = reserved != 0;`.
14. Abrir `/home/diogo/Zith/src/zirl/zirl-code-section.cpp`.
15. No `encodeCode`, existe uma sequência de flags de função: `is_extern`, `is_foreign_c`, `is_variadic`, `is_state`, `uses_tailcc`. Inserir `w.writeU8(fn.discardable ? 1 : 0);` logo depois de `uses_tailcc`.
16. No `decodeCode`, ler o byte novamente e fazer `fn.discardable = f != 0;` junto às flags lidas.
17. Abrir `/home/diogo/Zith/src/zirl/zirl-attrs-section.cpp`.
18. No `encodeAttrs`, dentro do loop de `attrs_fns`, trocar `w.writeU8(0);` (o primeiro dos três bytes reservados) por `w.writeU8(fn.discardable ? 1 : 0);`.
19. No `decodeAttrs`, utilizar `f` como `fn.discardable = f != 0;`.
20. Abrir `/home/diogo/Zith/src/session/persistent-cache.cpp`.
21. Na hidratação de `art.functions`, adicionar `fn.discardable = cfn.discardable;`.
22. Na hidratação de `art.attrs_fns`, adicionar `attrs.discardable = fn_attrs.discardable;`.
23. Abrir `/home/diogo/Zith/src/zirl/zirl-header.hpp`.
24. Adicionar o comentário `/// Version 17: source attribute metadata (discardable) is serialized and affects the public ABI hash.` e mudar `kFormatVersion` para `17`.

**Comando:**
```
cmake --build build -j4 --target zithc 2>&1 | tail -80
```

**Expected output:** Build passa e o artifact codifica/descodifica `discardable`.

**Failure checks:**
- Se `DeclRecord` é usado num teste de round-trip sem novo campo, o default `false` mantém compatibilidade de compilação; os testes novos atualizados no Step 13 devem incluir o campo.
- Se `CompactFunction` tem flags que o `decodeCode` lê em ordem diferente, seguir a ordem de escrita do `encodeCode` e reordenar os bytes de leitura para corresponder.
- Se o `encodeAttrs` tem três bytes reservados e o decode lê `a..h`, usar o primeiro byte para `discardable` e manter os restantes como leitura ignorada.

**Success criteria:** `rg -n "kFormatVersion = 17" /home/diogo/Zith/src/zirl/zirl-header.hpp` e um smoke test de cache com `#[discardable]` passa duas vezes (build e cache hit).

### Step 10 - Semântica volatile no SRA

**Goal:** Bindings `volatile` não geram factos SRA nem narrowing residual.

**Sub-steps:**
1. Abrir `/home/diogo/Zith/src/sema/sra-facts.cpp`.
2. Em `walkStatement`, depois de criar `SraLocalFact fact;`, adicionar:
```cpp
bool is_volatile = false;
for (const auto &attr : statement.binding.attributes)
    if (attr.kind == frontend::AttributeKind::Volatile)
        is_volatile = true;
if (is_volatile)
    return;
```
   Este `return` deve acontecer depois de `walkExpr(statement.binding.initializer)` para não impedir a validação do inicializador, mas antes de inserir em `local_facts_`.
3. Em `walkExpr`, nas ramificações de `Unary &`, `LayoutIntrinsic ptrOf`, `IsNull` e `walkConditionExpression`, ignorar locals volatile:
   - Criar helper `bool localIsVolatile(frontend::LocalId id) const noexcept` em `sra-facts.hpp`.
   - Implementar procurando em `current_module_->frontend->statements()` e `declarations_` por `binding.attributes` com `AttributeKind::Volatile`.
   - Nos pontos de narrowing, antes de escrever em `narrowing_facts_`, verificar `!localIsVolatile(local)`.
4. Não mudar factos para funções.

**Comando:**
```
cmake --build build -j4 --target zithc 2>&1 | tail -80
```

**Expected output:** Build passa e um binding `#[volatile] let p: ?*i32` seguido de `if (p != null)` não marca `p` como `nonNull` residual.

**Failure checks:**
- Se `current_module_` pode ser null em `localIsVolatile`, retornar false primeiro.

**Success criteria:** Teste de SRA/pipeline confirma que não há `HirSlotAttrs` residual para um slot volatile.

### Step 11 - `volatile` no codegen LLVM

**Goal:** Slots correspondentes a bindings `volatile` emitem load/store LLVM com `volatile`.

**Sub-steps:**
1. Abrir `/home/diogo/Zith/src/hir/hir-attrs.hpp`.
2. Adicionar a `struct HirSlotAttrs` um campo `bool volatileSlot = false;` depois de `nonNull`.
3. Confirmar que `hasResidualFacts()` de `HirSlotAttrs` NÃO inclui `volatileSlot`, para `volatile` não ser serializado no cache residual.
4. Abrir `/home/diogo/Zith/src/sema/hir-lower-stmt.cpp`.
5. No `StmtKind::Binding`, antes do alloca, marcar o slot quando a binding tem `AttributeKind::Volatile`:
```cpp
for (const auto &attr : statement.binding.attributes)
    if (attr.kind == frontend::AttributeKind::Volatile)
        hir_.attrs().slot(slot).volatileSlot = true;
```
   A marcação pode ir imediatamente após `localSlot(statement.binding.id)`, antes de `emitSlotAlloca`.
6. Abrir `/home/diogo/Zith/src/sema/hir-lower-modern.cpp`.
7. Na função `localSlot`, depois do alloca, também marcar `volatileSlot` para params se a declaração/função tiver atributos, mas no MVP `volatile` só é esperado em bindings; para params confirmar que `Parameter` não tem `attributes`, logo nenhum param é marcado.
8. Abrir `/home/diogo/Zith/src/codegen/codegen-emit-expr.cpp`.
9. Nos handlers de `HirSlotStore` e `HirSlotLoad` (linhas 129 e 183), consultar `hir_.attrs().trySlot(static_cast<hir::HirSlotId>(s.slot))`.
10. Quando o attrs existe e `attrs->volatileSlot` é true, usar a overload LLVM:
```cpp
builder_.CreateStore(val, slots_[s.slot], true);
builder_.CreateLoad(typeGen_.lower(s.type), slots_[s.slot], true);
```
   Se a API do LLVM 18 no projeto pede outro argumento, seguir o helper `CreateStore` já usado no mesmo ficheiro e verificar a assinatura com o compilador.
11. No MVP, não propagar `volatile` para ABI/cache; slots locais não são serializados por `hasResidualFacts`.
12. Não editar `HirSlotAddr`: o endereço de um slot volatile continua a ser o `alloca` normal.

**Comando:**
```
cmake --build build -j4 --target zithc 2>&1 | tail -80
```

**Expected output:** Build passa e codegen com LLVM gera loads/stores volatile para o binding marcado.

**Failure checks:**
- Se `LLVMFoldingBuilder` não aceita o terceiro argumento booleano, consultar as chamadas `CreateStore` e `CreateLoad` já existentes em `src/codegen` e usar a mesma assinatura.
- Se os handlers estão em outro ficheiro, usar `rg -n "HirSlotStore|HirSlotLoad" /home/diogo/Zith/src/codegen` para localizar.

**Success criteria:** Teste de codegen com `#[volatile] var x: i32 = 0; x = x + 1;` emite cargas/lojas volatile e corre com resultado correto.

### Step 12 - Formatter e reconstruct

**Goal:** `reconstruct()` preserva byte a byte os atributos originais; `FmtVisitor` mantém os grupos na mesma ordem.

**Sub-steps:**
1. Abrir `/home/diogo/Zith/src/frontend/frontend.cpp`.
2. `reconstruct()` já reescreve todos os tokens, por isso nada muda; confirmar que atributos ficam no source original.
3. Abrir `/home/diogo/Zith/src/formatter/fmt-visitor.cpp`.
4. Adicionar helper:
```cpp
void emitAttributes(const std::vector<frontend::Attribute> &attrs) {
    for (const auto &attr : attrs) {
        emit("#[");
        emit(attr.text);
        emit("]");
    }
}
```
5. Em `emitFunctionDecl`, chamar `emitAttributes(decl.attributes);` logo depois de `emitDeclPrefix(decl.span);` e antes de emitir `const `/`raw `/`extern `/`state `/`fn `.
6. Em `emitVariableDecl`, chamar `emitAttributes(decl.attributes);` depois de `emitDeclPrefix`.
7. Em `visitStmt`, caso `StmtKind::Binding`, chamar `emitAttributes(stmt->binding.attributes);` depois de `emitLeadingComments(first)` e antes de `emit(tokenText(first));`.
8. Adicionar espaços: após cada grupo emitir `" "`, para que `#[discardable] fn` fique legível.

**Comando:**
```
cmake --build build -j4 --target test-formatter 2>&1 | tail -80
./build/tests/test-formatter
```

**Expected output:** Testes de formatter passam.

**Failure checks:**
- Se `containsComment(decl.span)` faz fallback para `emitOriginal`, os atributos também são preservados por `emitOriginal`.

**Success criteria:** Novo teste de formatter compara `"#[discardable]   fn f() {}"` com `"#[discardable] fn f() {}"`.

### Step 13 - Testes

**Goal:** Testes cobrem parse, sema, SRA/codegen, cache/ZIRL e formatter.

**Sub-steps:**
1. Em `/home/diogo/Zith/tests/test-frontend.cpp`, adicionar `test_attribute_lex_and_parse()`:
   - `frontend::parse("#[discardable] #[volatile] fn f() {}")` tem 0 diagnostics.
   - `#[a, b] #[c]` tem 3 atributos na declaração.
   - `#[a = 1]` produz 1 diagnostic.
   - `reconstruct()` mantém a source exacta.
2. Em `/home/diogo/Zith/tests/test-frontend-modern-pipeline.cpp`, adicionar `test_discardable_attribute()`:
   - Ficheiro com `#[discardable] fn make(): i32 { return 1; }` e `fn main(): i32 { make(); return 0; }` deve passar `HirLowered` sem `DiscardedResult`.
   - Ficheiro sem o atributo deve continuar a falhar com `DiscardedResult`.
   - Binding local `#[volatile] var x: i32 = 1;` passa até `HirLowered`.
3. Em `/home/diogo/Zith/tests/test-cache.cpp`, adicionar teste de invalidação:
   - Gerar módulo com `#[discardable] fn f(): i32;`.
   - Compilar dependente que chama `f();`.
   - Remover o atributo e recompilar dependente; o ABI hash muda e o dependente volta a compilar/comporta-se igual.
4. Em `/home/diogo/Zith/tests/test-zirl-sections.cpp`, adicionar round-trip de `DeclRecord`/`CompactFunction`/`HirFnAttrsRecord` com `discardable = true`.
5. Em `/home/diogo/Zith/tests/test-formatter.cpp`, adicionar formatter para atributos em funções, variáveis globais e binding local.
6. Registar apenas funções já registadas no `main` de cada teste; não criar novos ficheiros de teste.

**Comando:**
```
cmake --build build -j4 --target zithc 2>&1 | tail -80
cmake --build build -j4 --target test-frontend test-frontend-modern-pipeline test-cache test-zirl-sections test-formatter 2>&1 | tail -120
./build/tests/test-frontend
./build/tests/test-frontend-modern-pipeline
./build/tests/test-cache
./build/tests/test-zirl-sections
./build/tests/test-formatter
```

**Expected output:** Todos os alvos terminam com código 0 e as suites mostram `Results: N passed, 0 failed`.

**Failure checks:**
- Se `test-frontend-modern-pipeline` tem alterações uncommitted do utilizador em `sema-zith.cpp`/`docs`, não editar esses ficheiros; ajustar apenas as funções novas no mesmo ficheiro de teste com `apply_patch`.
- Se `test-hir-lower-modern` falha com a falha pré-existente, ignorá-la e reportar como falha pré-existente.

**Success criteria:** As cinco suites novas passam sem alterar testes existentes que não sejam relevantes.

### Step 14 - Docs

**Goal:** `docs/Zith--.md` e `docs/Zith---implementation.md` documentam atributos, `discardable`, `volatile` e limitações MVP.

**Sub-steps:**
1. Abrir `/home/diogo/Zith/docs/Zith--.md`.
2. Adicionar secção `## Atributos` ou `## Attributes`:
   - Sintaxe `#[a, b]` e múltiplos grupos.
   - Posição antes de visibilidade e declaração.
   - `#[discardable]` só em funções, inclui Call e DockCall.
   - `#[volatile]` só em variáveis globais/locais, só marca o slot LLVM no MVP.
   - Atributos desconhecidos e não aplicáveis geram warnings.
   - Atributos preservados por formatter/reconstruct.
   - `assume`, `ensure`, `maybe` ficam fora do Zith--.
3. Abrir `/home/diogo/Zith/docs/Zith---implementation.md`.
4. Adicionar secção sobre lowering e cache:
   - AST `Attribute` com `kind`, `text`, `span`.
   - `ResolvedName.discardable`.
   - `HirFnAttrs.discardable`.
   - `volatile` marca `HirSlotAttrs.volatileSlot`, não viaja cross-module.
   - SRA ignora narrowing para slots volatile.
   - ZIRL version 17.
5. Não editar as linhas relacionadas com `let erro: Foo;` que já existem em docs.

**Comando:**
```
rg -n "Atributos|Attributes|discardable|volatile" /home/diogo/Zith/docs/Zith--.md /home/diogo/Zith/docs/Zith---implementation.md
```

**Expected output:** As novas palavras aparecem nas duas docs.

**Failure checks:**
- Se as docs já têm uma secção idêntica, atualizar em vez de duplicar.

**Success criteria:** `rg` encontra as três keywords em ambas as docs.

### Step 15 - Verificação final

**Goal:** Toda a mudança compila, os testes relevantes passam e a única falha conhecida é pré-existente.

**Sub-steps:**
1. Executar:
```
cmake --build build -j4 2>&1 | tail -100
ctest --test-dir build --output-on-failure 2>&1 | tail -120
```
2. Guardar o resultado completo em memória e reportar no final.
3. Se aparecerem falhas novas, corrigir antes de terminar.
4. Se `test-hir-lower-modern` falhar, confirmar que a mensagem é a do `opaque` cross-module e não tocar.

**Expected output:** Build global conclui e `ctest` tem 0 falhas novas.

**Failure checks:**
- Presença de `cannot convert '...' to '...'` indica erro de integração nova; corrigir no ficheiro indicado.
- Se `ctest` falha por `test-hir-lower-modern`, reportar como pré-existente.

**Success criteria:** `ctest --test-dir build --output-on-failure` termina com `0 tests failed` exceto a falha pré-existente conhecida e o executor consegue citar quais falhas restam.

## Acceptance final

Um agente com este plano deve conseguir:
1. Recolher o estado do repo.
2. Aplicar todos os steps com `apply_patch` (sem `cat > file`).
3. Verificar com os comandos dados.
4. Reportar que `#[discardable]` e `#[volatile]` funcionam no Zith--, que warnings por atributos inválidos usam códigos distintos, que cache/ZIRL versiona o formato e que docs estão atualizadas.
