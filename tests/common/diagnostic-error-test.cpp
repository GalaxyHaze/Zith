#include "diagnostic/error-info.hpp"
#include "common/diagnostic/render.hpp"
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
    using common::diagnostic::ErrorTemplate;
    using common::diagnostic::Severity;
    using common::diagnostic::errorInfo;
    using common::diagnostic::lookupError;
    using common::diagnostic::renderDiagnostic;
    using common::memory::SourceMap;
    using common::memory::SourceSpan;
    using common::memory::Span;

    const common::diagnostic::ErrorInfo *info = nullptr;
    if (!lookupError(4001, info) || info == nullptr) {
        return EXIT_FAILURE;
    }
    if (info->severity != Severity::Error || info->category != "compiler") {
        return EXIT_FAILURE;
    }
    if (info->title != "generic error") {
        return EXIT_FAILURE;
    }
    if (ErrorTemplate{info}.render("boom") != "boom") {
        return EXIT_FAILURE;
    }

    const common::diagnostic::ErrorInfo &lookup = errorInfo(4002);
    if (ErrorTemplate{&lookup}.render("", "foo") != "unknown foo") {
        return EXIT_FAILURE;
    }
    if (ErrorTemplate{nullptr}.render("raw") != "raw") {
        return EXIT_FAILURE;
    }

    const common::diagnostic::ErrorInfo *found = nullptr;
    if (!lookupError(4001, found) || found == nullptr) {
        return EXIT_FAILURE;
    }

    SourceMap map;
    const auto file = map.addFile("t.z", "x = a + y\n");
    if (!file) {
        return EXIT_FAILURE;
    }

    Diagnostic plain;
    plain.span = SourceSpan{file.value(), Span{0, 1}};
    plain.code = 4001;
    plain.message = "broken";
    FileCapture plainOut;
    renderDiagnostic(plainOut.ptr(), map, plain, {});
    const std::string plainText = plainOut.text();
    if (!contains(plainText, "t.z:1:1: error: E4001: broken")) {
        return EXIT_FAILURE;
    }
    if (!contains(plainText, "E4001: broken")) {
        return EXIT_FAILURE;
    }
    if (!contains(plainText, "= note: invalid source")) {
        return EXIT_FAILURE;
    }

    Diagnostic coded;
    coded.span = SourceSpan{file.value(), Span{0, 1}};
    coded.code = 4002;
    coded.message = "x";
    coded.notes.push_back(common::diagnostic::Note{"this name is not defined"});
    FileCapture codedOut;
    renderDiagnostic(codedOut.ptr(), map, coded, {});
    const std::string codedText = codedOut.text();
    if (!contains(codedText, "= note: this name is not defined")) {
        return EXIT_FAILURE;
    }

    Diagnostic plus;
    plus.span = SourceSpan{file.value(), Span{6, 7}};
    plus.code = 4002;
    FileCapture plusOut;
    renderDiagnostic(plusOut.ptr(), map, plus, {});
    const std::string plusText = plusOut.text();
    if (!contains(plusText, "unknown +")) {
        std::fprintf(stderr, "FAIL: lexeme placeholder did not include '+' \n%s\n", plusText.c_str());
        return EXIT_FAILURE;
    }

    Diagnostic invalid;
    invalid.span = SourceSpan{9999, Span{0, 1}};
    invalid.code = 4002;
    FileCapture invalidOut;
    renderDiagnostic(invalidOut.ptr(), map, invalid, {});
    const std::string invalidText = invalidOut.text();
    if (!contains(invalidText, "<invalid span>") || !contains(invalidText, "unknown <invalid span>")) {
        std::fprintf(stderr, "FAIL: invalid span fallback missing\n%s\n", invalidText.c_str());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
