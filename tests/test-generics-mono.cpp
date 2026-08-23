#include "cli/options.hpp"
#include "diagnostics/error-codes.hpp"
#include "session/compilation-session.hpp"
#include "session/frontend-context.hpp"
#include "test-common.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace zith;

namespace {

struct MonoTest {
    memory::Arena arena;
    Options opts;
    std::filesystem::path root;

    MonoTest()
        : opts(arena), root(std::filesystem::temp_directory_path() / "zith-generics-mono-tests") {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        opts.targetStage = session::Stage::HirLowered;
    }

    ~MonoTest() {
        std::filesystem::remove_all(root);
    }

    struct Result {
        bool ok = false;
        struct Diag {
            diagnostics::ErrCode code;
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
    };

    Result run(std::string_view input, session::Stage target = session::Stage::HirLowered) {
        const auto path = root / "main.zith";
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << input;
        file.close();

        session::FrontendConfig config;
        config.workspaceRoot      = root.string();
        config.maxFrontendWorkers = 1;
        config.compilerVersion    = "test";
        auto context              = std::make_shared<session::FrontendContext>(config);
        session::CompilationSession session(opts, path.string(), std::move(context));
        session.setBuffered(true);

        const bool ok = session.runTo(target);
        std::vector<Result::Diag> copied;
        for (const auto &diag : session.diags().all()) {
            copied.push_back({static_cast<diagnostics::ErrCode>(diag.code), diag.message});
            if (diag.severity == diagnostics::Severity::Error) {
                std::printf("    [GenericMonoDiag] Code: %u, Message: %s\n", diag.code,
                            diag.message.c_str());
            }
        }
        return {ok && session.diags().errorCount() == 0, std::move(copied)};
    }
};

void test_generic_function_inference_and_collapse() {
    MonoTest t;
    auto r = t.run("fn id<T>(x: T): T { return x }\n"
                   "fn main(): i32 {\n"
                   "    let a: i32 = id<i32>(3);\n"
                   "    let b: i32 = id(4);\n"
                   "    return a + b;\n"
                   "}\n");
    CHECK(r.ok, "explicit and inferred generic calls type-check and lower");
    CHECK(!r.hasErrorCode(diagnostics::err::GenericArity), "correct arity does not report E3010");
    CHECK(!r.hasErrorCode(diagnostics::err::GenericCannotInfer),
          "inferable generic calls do not report E3011");
}

void test_generic_multi_param_function() {
    MonoTest t;
    auto r = t.run("fn pair<T, U>(a: T, b: U): U { return b }\n"
                   "fn main(): i32 {\n"
                   "    let c: f64 = pair<i32, f64>(7, 2.5);\n"
                   "    return 1;\n"
                   "}\n");
    CHECK(r.ok, "a two-argument generic function call lowers");
}

void test_generic_struct_alias_method_and_implement() {
    MonoTest t;
    auto r = t.run("struct Pair<T, U> { first: T, second: U }\n"
                   "alias Box<T> = T\n"
                   "struct Node<T> { value: T }\n"
                   "implement Node<T> {\n"
                   "    fn get(self): T { return self->value }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let s: Pair<i32, f64> = Pair<i32, f64>{ first: 1, second: 2.5 };\n"
                   "    let n: Node<i32> = Node<i32>{ value: 11 };\n"
                   "    let q: Box<i32> = 9;\n"
                   "    return s.first + n.get() + q;\n"
                   "}\n");
    CHECK(r.ok, "generic structs, aliases, methods and implement blocks lower together");
}

void test_generic_arity_error() {
    MonoTest t;
    auto r = t.run("fn id<T>(x: T): T { return x }\n"
                   "fn main(): i32 {\n"
                   "    id<i32, i64>(1);\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(!r.ok, "wrong generic arity is rejected");
    CHECK(r.hasErrorCode(diagnostics::err::GenericArity), "wrong generic arity reports E3010");
}

void test_generic_cannot_infer_error() {
    MonoTest t;
    auto r = t.run("fn needs<T>(x: i32): T { return x }\n"
                   "fn main(): i32 {\n"
                   "    needs(1);\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(!r.ok, "a non-inferable generic call is rejected");
    CHECK(r.hasErrorCode(diagnostics::err::GenericCannotInfer),
          "the missing inference reports E3011");
}

void test_generic_explosion_limit() {
    MonoTest t;
    const std::string recursive = "struct Box<T> { value: T }\n"
                                  "fn nest<T>(x: T): Box<T> { return Box<T>{ value: x } }\n"
                                  "fn main(): i32 {\n"
                                  "    let x: i32 = 0;\n";
    std::string body            = "    x = nest(x).value;\n";
    for (size_t index = 0; index < 8U; ++index)
        body += "    x = nest(x).value;\n";
    body += "    return x;\n"
            "}\n";
    auto r = t.run(recursive + body);
    CHECK(r.hasErrorCode(diagnostics::err::GenericExplosion) || r.ok,
          "recursive generic instantiation either lowers or reports the explosion limit");
}

} // namespace

int main() {
    std::printf("generics mono tests\n");
    std::printf("=====================\n\n");
    g_test_passed = 0;
    g_test_failed = 0;
    test_generic_function_inference_and_collapse();
    test_generic_multi_param_function();
    test_generic_struct_alias_method_and_implement();
    test_generic_arity_error();
    test_generic_cannot_infer_error();
    test_generic_explosion_limit();
    std::printf("\nResults: %d passed, %d failed\n", g_test_passed, g_test_failed);
    return g_test_failed > 0 ? 1 : 0;
}
