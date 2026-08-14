#include "common/diagnostic/diagnostic.hpp"
#include "common/diagnostic/render.hpp"
#include "common/memory/source-map.hpp"
#include "diagnostic/error-info.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

using common::diagnostic::Diagnostic;
using common::diagnostic::ErrorTemplate;
using common::diagnostic::lookupError;
using common::diagnostic::renderDiagnostic;
using common::memory::SourceMap;
using common::memory::SourceSpan;
using common::memory::Span;

namespace {

bool expect(bool condition, std::string_view message) {
    if (!condition)
        std::cerr << "diagnostic-demo: " << message << '\n';
    return condition;
}

} // namespace

int main() {
    const common::diagnostic::ErrorInfo *info = nullptr;
    if (!expect(lookupError(4001, info) && info != nullptr, "E4001 lookup"))
        return EXIT_FAILURE;

    std::cout << "E4001 " << info->category << " / " << info->title << " -> "
              << ErrorTemplate{info}.render("broken") << "\n";

    const auto &lookup = common::diagnostic::errorInfo(4002);
    std::cout << "E4002 " << lookup.category << " / " << lookup.title << " -> "
              << ErrorTemplate{&lookup}.render("", "missing_name") << "\n";

    SourceMap map;
    const auto file = map.addFile("demo.turv", "let x = missing_name + 0;\n");
    if (!expect(file.isOk(), "source file added"))
        return EXIT_FAILURE;

    Diagnostic diag;
    diag.span = SourceSpan{file.value(), Span{8, 20}};
    diag.code = 4002;
    diag.message = "missing_name";
    renderDiagnostic(stdout, map, diag, {});

    std::cout << "diagnostic-demo: E4001 and E4002 are present\n";
    return EXIT_SUCCESS;
}
