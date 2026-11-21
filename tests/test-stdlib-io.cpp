#include "cli/options.hpp"
#include "diagnostics/error-codes.hpp"
#include "session/compilation-session.hpp"
#include "session/frontend-context.hpp"
#include "test-common.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace zith;

namespace {

struct SessionRunner {
    memory::Arena arena;
    Options opts;
    std::filesystem::path root;

    SessionRunner()
        : opts(arena), root(std::filesystem::temp_directory_path() / "zith-stdlib-io-tests") {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        opts.targetStage = session::Stage::HirLowered;
#ifdef ZITH_STDLIB_DIR
        opts.includeDirs.push(ZITH_STDLIB_DIR);
#endif
    }

    ~SessionRunner() {
        std::filesystem::remove_all(root);
    }

    struct Result {
        bool ok = false;
        struct Diag {
            diagnostics::ErrCode code{};
            std::string message;
        };
        std::vector<Diag> diags;

        [[nodiscard]] bool hasErrorCode(diagnostics::ErrCode code) const {
            for (const auto &diag : diags) {
                if (diag.code == code)
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool hasMessage(std::string_view needle) const {
            for (const auto &diag : diags) {
                if (diag.message.find(needle) != std::string::npos)
                    return true;
            }
            return false;
        }
    };

    Result run(std::string_view input, session::Stage target = session::Stage::HirLowered) {
        return run(input, target, std::string{});
    }

    Result run(std::string_view input, session::Stage target, std::string targetTriple) {
        const auto path = root / "main.zithm";
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << input;
        file.close();

        session::FrontendConfig config;
        config.workspaceRoot      = root.string();
        config.maxFrontendWorkers = 1;
        config.compilerVersion    = "test";
        config.targetTriple       = std::move(targetTriple);
        for (const auto &dir : opts.includeDirs)
            config.includeRoots.push_back(dir);
        auto context = std::make_shared<session::FrontendContext>(config);
        session::CompilationSession session(opts, path.string(), std::move(context));
        session.setBuffered(true);

        const bool ok = session.runTo(target);
        Result result;
        result.ok = ok && session.diags().errorCount() == 0;
        for (const auto &diag : session.diags().all()) {
            result.diags.push_back({static_cast<diagnostics::ErrCode>(diag.code), diag.message});
            if (diag.severity == diagnostics::Severity::Error) {
                std::printf("    [StdlibIoDiag] Code: %u, Message: %s\n", diag.code,
                            diag.message.c_str());
            }
        }
        return result;
    }
};

void test_dyn_textsink_simple_receiver() {
    SessionRunner t;
    auto r = t.run("trait TextSink {\n"
                   "    fn capacity(self): u64;\n"
                   "    fn text(self): []char;\n"
                   "}\n"
                   "struct ByteBuffer {\n"
                   "    data: []char,\n"
                   "}\n"
                   "implement ByteBuffer as TextSink {\n"
                   "    fn capacity(self): u64 { return 0; }\n"
                   "    fn text(self): []char { return self.data; }\n"
                   "}\n"
                   "fn write(d: dyn TextSink) {\n"
                   "    let c = d.capacity();\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    var b = ByteBuffer { data: \"\" };\n"
                   "    write(b);\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(r.ok, "dyn TextSink with simple trait receivers compiles");
}

void test_qualified_trait_receiver_is_currently_blocked() {
    SessionRunner t;
    auto r = t.run("trait TextSink {\n"
                   "    fn capacity(self: view Self): u64;\n"
                   "}\n"
                   "struct ByteBuffer {\n"
                   "    data: []char,\n"
                   "}\n"
                   "implement ByteBuffer as TextSink {\n"
                   "    fn capacity(self: view Self): u64 { return 0; }\n"
                   "}\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(!r.ok, "qualified trait receiver remains a recorded compiler blocker");
    CHECK(r.hasErrorCode(diagnostics::err::TraitMethodSignatureMismatch),
          "qualified receiver fails with E2022");
}

void test_lend_dyn_trait_receiver_compiles() {
    SessionRunner t;
    auto r = t.run("pub trait TextSink {\n"
                   "    fn capacity(self): u64;\n"
                   "    fn append(self: lend dyn TextSink, chars: []char): u64;\n"
                   "    fn text(self): []char;\n"
                   "}\n"
                   "struct ByteBuffer {\n"
                   "    data: []char,\n"
                   "}\n"
                   "implement ByteBuffer as TextSink {\n"
                   "    fn capacity(self): u64 { return 0; }\n"
                   "    fn append(self: lend ByteBuffer, chars: []char): u64 { return 0; }\n"
                   "    fn text(self): []char { return self.data; }\n"
                   "}\n"
                   "fn write(d: dyn TextSink) {\n"
                   "    let c = d.capacity();\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    var b = ByteBuffer { data: \"\" };\n"
                   "    write(b);\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(r.ok, "a lend dyn Trait receiver compiles in sema");
    CHECK(!r.hasErrorCode(diagnostics::err::TraitMethodSignatureMismatch),
          "the lend dyn receiver no longer fails with the ownership receiver diagnostic");
}

void test_variadic_dyn_formatable_uses_bridge() {
    SessionRunner t;
    auto r = t.run("pub trait Formatable {\n"
                   "    fn format(self): []char;\n"
                   "}\n"
                   "struct S {}\n"
                   "implement S as Formatable {\n"
                   "    fn format(self): []char { return \"x\"; }\n"
                   "}\n"
                   "fn get(v: dyn Formatable): []char { return v.format(); }\n"
                   "fn writeValues(msg: []char, values: [...]dyn Formatable) {\n"
                   "    var i: u64 = 0;\n"
                   "    let n = @lengthOf(values);\n"
                   "    for (i < n) {\n"
                   "        let s = get(raw values[i]);\n"
                   "        i = i + 1;\n"
                   "    }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    writeValues(\"hello\", S{});\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(r.ok, "variadic dyn Formatable call through a bridge compiles");
}

void test_generic_union_result_helper() {
    SessionRunner t;
    auto r = t.run("union Result<T, E> {\n"
                   "    T,\n"
                   "    E,\n"
                   "}\n"
                   "fn Ok<T, E>(value: T): Result<T, E> {\n"
                   "    var r: Result<T, E> = Result<T, E>{ value };\n"
                   "    return r;\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let a: Result<u32, u32> = Ok<u32, u32>(7);\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(r.ok, "a generic Ok helper returning a positional union now compiles");
}

void test_generic_union_direct_construction_blocked() {
    SessionRunner t;
    auto r = t.run("union Result<T, E> {\n"
                   "    T,\n"
                   "    E,\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    var r: Result<u32, u32> = Result<u32, u32>{ 7 };\n"
                   "    let n: u32 = raw r as u32;\n"
                   "    return n as i32 - 7;\n"
                   "}\n");
    CHECK(r.ok, "direct generic union construction plus raw cast works");
}

void test_io_format_returns_owned_result() {
    SessionRunner t;
    auto r = t.run("from std/io/format\n"
                   "from soon/memory\n"
                   "fn main(): i32 {\n"
                   "    let heap = HeapAllocator {};\n"
                   "    let result = format(heap, \"n=#\", 7);\n"
                   "    let status = result.error();\n"
                   "    var text = result.text();\n"
                   "    result.destroy();\n"
                   "    if (status != IoError.Ok) { return 1; }\n"
                   "    return (@lengthOf(text) as i32) - 2;\n"
                   "}\n");
    CHECK(r.ok, "owned format result is usable through std/io/format");
}

void test_io_format_into_existing_buffer() {
    SessionRunner t;
    auto r = t.run("from std/io/format\n"
                   "from soon/memory\n"
                   "fn main(): i32 {\n"
                   "    let heap = HeapAllocator {};\n"
                   "    var buffer = emptyFormatBuffer(heap);\n"
                   "    let status = format(lend buffer, \"n=#\", 7);\n"
                   "    var text = buffer.text();\n"
                   "    buffer.destroy();\n"
                   "    if (status != IoError.Ok) { return 1; }\n"
                   "    return (@lengthOf(text) as i32) - 2;\n"
                   "}\n");
    CHECK(r.ok, "format can append into an existing FormatBuffer");
}

void test_c_fs_portable_surface_checks() {
    SessionRunner t;
    auto r = t.run("from c/fs\n"
                   "from c/stdlib\n"
                   "fn main(): i32 {\n"
                   "    let stream = fopen(\"/tmp/zith-fs-test.txt\", \"wb\");\n"
                   "    if (stream is null) { return 1; }\n"
                   "    defer fclose(stream);\n"
                   "    let msg: *char = \"data\";\n"
                   "    let written = fwrite(msg, 1u64, 4u64, stream);\n"
                   "    let pos = ftell(stream);\n"
                   "    _ = fflush(stream);\n"
                   "    if (written != 4u64 or pos < 0i64) { return 2; }\n"
                   "    _ = fseek(stream, 0i64, SEEK_SET);\n"
                   "    var buf: ?raw opaque = malloc(8u64);\n"
                   "    if (buf is null) { return 3; }\n"
                   "    defer free(buf);\n"
                   "    let read_count = fread(buf, 1u64, 4u64, stream);\n"
                   "    if (read_count != 4u64) { return 4; }\n"
                   "    _ = feof(stream);\n"
                   "    _ = ferror(stream);\n"
                   "    _ = remove(\"/tmp/zith-fs-test.txt\");\n"
                   "    _ = rename(\"/tmp/zith-fs-a.txt\", \"/tmp/zith-fs-b.txt\");\n"
                   "    0\n"
                   "}\n");
    CHECK(r.ok, "c/fs portable stdio surface type-checks");
}

void test_c_fs_resolves_on_a_non_linux_target() {
    SessionRunner t;
    auto r = t.run("from c/fs\n"
                   "fn main(): i32 { return 0; }\n",
                   session::Stage::HirLowered, "x86_64-pc-windows-msvc");
    CHECK(r.ok, "c/fs has no platform variant and resolves on every target");
}

void test_c_posix_variant_resolves_on_linux() {
    SessionRunner t;
    auto r = t.run("from c/posix\n"
                   "from c/fs\n"
                   "from c/stdlib\n"
                   "fn main(): i32 {\n"
                   "    let fd = open(\"/tmp/zith-fs-test.txt\", O_RDONLY);\n"
                   "    if (fd < 0) { return 1; }\n"
                   "    defer close(fd);\n"
                   "    var buf: ?raw opaque = malloc(8u64);\n"
                   "    if (buf is null) { return 2; }\n"
                   "    defer free(buf);\n"
                   "    let got = read(fd, buf, 8u64);\n"
                   "    let mapped = mmap(null, 8u64, PROT_READ, MAP_PRIVATE, fd, 0i64);\n"
                   "    if (mapped is null) { return 3; }\n"
                   "    defer munmap(raw mapped, 8u64);\n"
                   "    _ = lseek(fd, 0i64, SEEK_SET);\n"
                   "    let present = access(\"/tmp\", F_OK);\n"
                   "    _ = unlink(\"/tmp/zith-fs-none.txt\");\n"
                   "    var info: ?raw opaque = malloc(STAT_BUFFER_SIZE);\n"
                   "    if (info is null) { return 4; }\n"
                   "    defer free(info);\n"
                   "    _ = stat(\"/tmp\", info);\n"
                   "    _ = fstat(fd, info);\n"
                   "    if (got < -1i64 or present < -1) { return 5; }\n"
                   "    0\n"
                   "}\n",
                   session::Stage::HirLowered, "x86_64-unknown-linux-gnu");
    CHECK(r.ok, "c/posix resolves and type-checks on the linux platform variant");
}

void test_c_posix_missing_variant_reports_import_error() {
    SessionRunner t;
    auto r = t.run("from c/posix\n"
                   "fn main(): i32 { return 0; }\n",
                   session::Stage::HirLowered, "x86_64-pc-windows-msvc");
    CHECK(!r.ok, "c/posix does not resolve on a target without a matching variant");
    CHECK(r.hasErrorCode(diagnostics::err::ImportError),
          "the missing platform variant reports E1006");
}

} // namespace

int main() {
    std::printf("stdlib io tests\n");
    std::printf("===============\n\n");
    g_test_passed = 0;
    g_test_failed = 0;
    test_dyn_textsink_simple_receiver();
    test_qualified_trait_receiver_is_currently_blocked();
    test_lend_dyn_trait_receiver_compiles();
    test_variadic_dyn_formatable_uses_bridge();
    test_generic_union_result_helper();
    test_generic_union_direct_construction_blocked();
    test_io_format_returns_owned_result();
    test_io_format_into_existing_buffer();
    test_c_fs_portable_surface_checks();
    test_c_fs_resolves_on_a_non_linux_target();
    test_c_posix_variant_resolves_on_linux();
    test_c_posix_missing_variant_reports_import_error();
    std::printf("\nResults: %d passed, %d failed\n", g_test_passed, g_test_failed);
    return g_test_failed > 0 ? 1 : 0;
}
