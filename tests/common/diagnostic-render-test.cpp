#include "common/diagnostic/render.hpp"

#include "common/diagnostic/diagnostic.hpp"
#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"
#include "common/memory/source-map.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

class FileCapture {
public:
    FileCapture() : file_(std::tmpfile()) {}
    ~FileCapture() {
        if (file_)
            std::fclose(file_);
    }

    FileCapture(const FileCapture &) = delete;
    FileCapture &operator=(const FileCapture &) = delete;

    auto ptr() noexcept -> FILE * { return file_; }

    auto text() -> std::string {
        if (!file_)
            return {};
        std::rewind(file_);
        char buffer[1024];
        std::string result;
        while (std::fgets(buffer, sizeof(buffer), file_))
            result.append(buffer);
        return result;
    }

private:
    FILE *file_;
};

bool contains(const std::string &text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

int main() {
    using common::diagnostic::Diagnostic;
    using common::diagnostic::RenderOptions;
    using common::diagnostic::renderDiagnostic;
    using common::diagnostic::renderDiagnostics;
    using common::diagnostic::Severity;
    using common::memory::Arena;
    using common::memory::DynArray;
    using common::memory::Span;
    using common::memory::SourceMap;
    using common::memory::SourceSpan;

    SourceMap map;
    const auto add = map.addFile("src/main.zith", "let y = 0 ;\n let x = a + ;\nlet z = 0 ;\n");
    if (!add)
        return EXIT_FAILURE;

    const Diagnostic diag{
        .span = SourceSpan{add.value(), Span{23, 25}},
        .severity = Severity::Error,
        .message = "unexpected token '+'",
    };

    FileCapture plain;
    renderDiagnostic(plain.ptr(), map, diag, RenderOptions{false, 1});
    const std::string plainOut = plain.text();

    if (!contains(plainOut, "src/main.zith:2:12: error: unexpected token '+'")) {
        std::fprintf(stderr, "FAIL: missing location line\n%s\n", plainOut.c_str());
        return EXIT_FAILURE;
    }
    if (!contains(plainOut, "^")) {
        std::fprintf(stderr, "FAIL: missing caret\n");
        return EXIT_FAILURE;
    }
    if (plainOut.find("\x1b[") != std::string::npos) {
        std::fprintf(stderr, "FAIL: ANSI escape emitted without color\n");
        return EXIT_FAILURE;
    }

    const Diagnostic coded{
        .span = SourceSpan{add.value(), Span{23, 24}},
        .severity = Severity::Error,
        .code = 4002,
        .message = "x",
    };

    FileCapture codedOut;
    renderDiagnostic(codedOut.ptr(), map, coded, RenderOptions{false, 1});
    const std::string codedText = codedOut.text();
    if (!contains(codedText, "src/main.zith:2:12: error: E4002: unknown +")) {
        std::fprintf(stderr, "FAIL: missing coded compact line\n%s\n", codedText.c_str());
        return EXIT_FAILURE;
    }
    if (!contains(codedText, "  --> src/main.zith:2:12\n")) {
        std::fprintf(stderr, "FAIL: missing coded rich header\n%s\n", codedText.c_str());
        return EXIT_FAILURE;
    }
    if (!contains(codedText, "= note: check the spelling")) {
        std::fprintf(stderr, "FAIL: missing catalogue note\n%s\n", codedText.c_str());
        return EXIT_FAILURE;
    }

    Diagnostic codedWithNote = coded;
    codedWithNote.notes.push_back(common::diagnostic::Note{"this name is not defined"});
    FileCapture notesOut;
    renderDiagnostic(notesOut.ptr(), map, codedWithNote, RenderOptions{false, 1});
    const std::string notesText = notesOut.text();
    if (!contains(notesText, "= note: check the spelling")
        || !contains(notesText, "= note: this name is not defined")) {
        std::fprintf(stderr, "FAIL: missing coded notes\n%s\n", notesText.c_str());
        return EXIT_FAILURE;
    }

    FileCapture context;
    renderDiagnostic(context.ptr(), map, diag, RenderOptions{false, 1});
    const std::string contextOut = context.text();
    if (!contains(contextOut, "  1 | let y = 0 ;\n")) {
        std::fprintf(stderr, "FAIL: missing previous context line\n%s\n", contextOut.c_str());
        return EXIT_FAILURE;
    }
    if (!contains(contextOut, "  3 | let z = 0 ;\n")) {
        std::fprintf(stderr, "FAIL: missing next context line\n%s\n", contextOut.c_str());
        return EXIT_FAILURE;
    }
    if (!contains(contextOut, "|")) {
        std::fprintf(stderr, "FAIL: missing context gutter\n%s\n", contextOut.c_str());
        return EXIT_FAILURE;
    }

    FileCapture colored;
    renderDiagnostic(colored.ptr(), map, diag, RenderOptions{true, 0});
    const std::string coloredOut = colored.text();
    if (coloredOut.find("\x1b[31m") == std::string::npos) {
        std::fprintf(stderr, "FAIL: missing error color sequence\n");
        return EXIT_FAILURE;
    }
    if (!contains(coloredOut, "\x1b[0m")) {
        std::fprintf(stderr, "FAIL: missing reset sequence\n");
        return EXIT_FAILURE;
    }

    Arena arena;
    DynArray<Diagnostic> diags{arena};
    diags.push(diag);
    diags.push(diag);
    FileCapture capped;
    renderDiagnostics(capped.ptr(), map, diags, {}, 1);
    const std::string cappedOut = capped.text();
    const std::size_t first = cappedOut.find("unexpected token");
    const std::size_t second = cappedOut.find("unexpected token", first + 1);
    if (first == std::string::npos || second != std::string::npos) {
        std::fprintf(stderr, "FAIL: maxErrors did not cap output\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
