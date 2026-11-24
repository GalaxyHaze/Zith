#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Stable IDE-facing API. The public header intentionally does not include
// compiler internals; implementations may use them privately in the .cpp.
namespace zith::ide {

enum class FeatureStability : uint8_t {
    Stable,
    Experimental,
    Planned,
};

enum class QueryKind : uint8_t {
    Diagnostics,
    Completion,
    CompletionResolve,
    Hover,
    SignatureHelp,
    Definition,
    References,
    DocumentSymbols,
    WorkspaceSymbols,
    SemanticTokensFull,
    SemanticTokensDelta,
    InlayHints,
    Highlights,
    PrepareRename,
    Rename,
    Formatting,
    Folding,
    CodeActions,
    Commands,
    Configuration,
};

struct Position {
    uint32_t line = 0;
    uint32_t character = 0;
};

struct Range {
    Position start;
    Position end;
};

enum class Severity : uint8_t {
    Note,
    Warning,
    Error,
    Bug,
};

struct Diagnostic {
    Range range;
    Severity severity = Severity::Error;
    uint32_t code = 0;
    std::string message;
    std::string source = "zithc";
    std::vector<std::string> suggestions;
};

struct CompletionItem {
    std::string label;
    int kind = 0;
    std::string detail;
    std::string insertText;
    int insertTextFormat = 1;
    std::string documentation;
    std::string dataKind;
    std::string dataName;
    std::string dataOwner;
};

struct SemanticToken {
    uint32_t deltaLine = 0;
    uint32_t deltaStart = 0;
    uint32_t length = 0;
    uint32_t tokenType = 0;
    uint32_t tokenModifiers = 0;
};

struct InlayHint {
    Position position;
    std::string label;
};

struct DocumentSymbol {
    std::string name;
    int kind = 0;
    /// Short annotation shown next to the name in an outline. For a named
    /// block it carries the marker and its resource arguments, so a reader
    /// sees what the phase brackets without opening the body.
    std::string detail;
    Range range;
    Range selectionRange;
    std::vector<DocumentSymbol> children;
};

struct Location {
    std::string uri;
    Range range;
};

struct TextEdit {
    Range range;
    std::string newText;
};

struct ParameterInfo {
    std::string label;
};

struct SignatureInfo {
    std::string label;
    std::vector<ParameterInfo> parameters;
};

struct SignatureHelp {
    std::vector<SignatureInfo> signatures;
    int activeSignature = 0;
    int activeParameter = 0;
};

struct HoverResult {
    std::string markdown;
};

struct Highlight {
    Range range;
    int kind = 1;
};

struct FoldingRange {
    uint32_t startLine = 0;
    uint32_t startCharacter = 0;
    uint32_t endLine = 0;
    uint32_t endCharacter = 0;
};

struct CodeAction {
    std::string title;
    std::string kind;
    std::optional<std::string> editDocumentUri;
    std::vector<TextEdit> edits;
};

struct DocumentSnapshot {
    std::string uri;
    std::string filePath;
    std::string text;
    int64_t version = 0;
    uint64_t revision = 0;
};

struct Query {
    QueryKind kind = QueryKind::Diagnostics;
    uint32_t line = 0;
    uint32_t character = 0;
    Range range;
    std::string text; // rename target or completion item label
    std::string dataKind;
    std::string dataName;
    std::string dataOwner;
    std::string workspaceQuery;
};

struct AnalysisResult {
    QueryKind kind = QueryKind::Diagnostics;
    uint64_t analyzedRevision = 0;
    bool ok = false;
    bool cancelled = false;
    std::string error;

    std::vector<Diagnostic> diagnostics;
    std::vector<CompletionItem> completions;
    std::vector<Location> definitions;
    std::vector<Location> references;
    std::vector<DocumentSymbol> documentSymbols;
    std::vector<DocumentSymbol> workspaceSymbols;
    std::vector<SemanticToken> semanticTokens;
    std::vector<InlayHint> inlayHints;
    std::vector<Highlight> highlights;
    std::vector<TextEdit> formatting;
    std::vector<FoldingRange> folding;
    std::vector<CodeAction> codeActions;
    std::vector<std::string> commands;
    std::unordered_map<std::string, std::string> configuration;

    std::optional<HoverResult> hover;
    std::optional<SignatureHelp> signatureHelp;
    std::optional<Range> prepareRename;
    std::unordered_map<std::string, std::vector<TextEdit>> renameChanges;
};

struct WorkspaceConfig {
    std::string workspaceRoot;
    std::string stdlibPath;
    std::string compilerVersion = "unknown";
    size_t maxWorkers = 0;
};

// Owns shared compiler context. Copying is intentionally not supported.
class Workspace {
public:
    explicit Workspace(WorkspaceConfig config);
    ~Workspace();

    Workspace(const Workspace &) = delete;
    Workspace &operator=(const Workspace &) = delete;

    DocumentSnapshot openDocument(std::string uri, std::string filePath,
                                  std::string text, int64_t version);
    DocumentSnapshot changeDocument(const std::string &uri,
                                    const std::string &text,
                                    int64_t version);
    void closeDocument(const std::string &uri) noexcept;
    std::optional<DocumentSnapshot> snapshotFor(const std::string &uri) const;

    AnalysisResult analyze(const DocumentSnapshot &snapshot, const Query &query);

    [[nodiscard]] FeatureStability stability(QueryKind kind) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zith::ide
