#include "common/memory/source-map.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

using common::memory::FileId;
using common::memory::Loc;
using common::memory::SourceMap;
using common::memory::SourceSpan;

namespace {

bool check(bool ok, std::string_view message) {
    if (!ok)
        std::fprintf(stderr, "FAIL: %.*s\n", static_cast<int>(message.size()), message.data());
    return ok;
}

bool check_loc(Loc got, uint32_t line, uint32_t col, std::string_view message) {
    if (got.line == line && got.col == col)
        return true;
    std::fprintf(stderr, "FAIL: %.*s: got %u:%u, want %u:%u\n",
                 static_cast<int>(message.size()), message.data(),
                 got.line, got.col, line, col);
    return false;
}

} // namespace

int main() {
    bool ok = true;

    SourceMap map;
    const auto add = map.addFile("test.zith", "hello\nworld");
    ok &= check(add.isOk(), "addFile returns ok");
    if (!add.isOk())
        return EXIT_FAILURE;

    const FileId id = add.value();
    ok &= check(map.exists(id), "added file is valid");
    ok &= check(!map.exists(UINT32_MAX), "invalid id is not valid");

    const auto first = map.snippet(SourceSpan{id, {0, 5}});
    ok &= check(first.isOk() && first.value() == "hello", "snippet keeps first five bytes");

    const auto second = map.snippet(SourceSpan{id, {6, 11}});
    ok &= check(second.isOk() && second.value() == "world", "snippet keeps second line");

    ok &= check_loc(map.loc(SourceSpan{id, {0, 0}}), 1, 1, "offset 0 is line 1 col 1");
    ok &= check_loc(map.loc(SourceSpan{id, {6, 6}}), 2, 1, "offset 6 is line 2 col 1");
    ok &= check_loc(map.loc(SourceSpan{id, {7, 7}}), 2, 2, "offset 7 is line 2 col 2");

    const auto same = map.addFile("other.zith", "aaa");
    ok &= check(same.isOk() && same.value() != id, "distinct path yields distinct id");
    ok &= check_loc(map.loc(SourceSpan{UINT32_MAX, {0, 0}}), 0, 0, "invalid file yields zero loc");

    const auto missing = map.loadFile("/nonexistent/zith/source.zith");
    ok &= check(missing.isError(), "missing file returns error");

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
