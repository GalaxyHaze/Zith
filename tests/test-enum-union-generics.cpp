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

struct GenericEnumUnionTest {
    memory::Arena arena;
    Options opts;
    std::filesystem::path root;

    GenericEnumUnionTest()
        : opts(arena),
          root(std::filesystem::temp_directory_path() / "zith-enum-union-generics-tests") {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        opts.targetStage = session::Stage::HirLowered;
    }

    ~GenericEnumUnionTest() {
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
                std::printf("    [EnumUnionGenericDiag] Code: %u, Message: %s\n", diag.code,
                            diag.message.c_str());
            }
        }
        return {ok && session.diags().errorCount() == 0, std::move(copied)};
    }
};

void test_inline_generic_enum_method() {
    GenericEnumUnionTest t;
    auto r = t.run(
        "enum Status<T> { Ok = 0, Err = 1, fn code(self): i32 { return @sizeOf(T) as i32; } }\n"
        "fn main(): i32 {\n"
        "    let s: Status<i32> = Status.Ok;\n"
        "    return s.code();\n"
        "}\n");
    CHECK(r.ok, "a generic enum with an inline method types and lowers");
}

void test_inline_generic_union_method() {
    GenericEnumUnionTest t;
    auto r = t.run("union Any<T, U> { T, U, fn pick(self): T { return raw self as T; } }\n"
                   "fn main(): i32 {\n"
                   "    var a: Any<i32, f64> = Any<i32, f64>{ 42 };\n"
                   "    return a.pick();\n"
                   "}\n");
    CHECK(r.ok, "a generic union with an inline raw-as method types and lowers");
}

void test_generic_implement_enum_and_union() {
    GenericEnumUnionTest t;
    auto r =
        t.run("trait Value { fn value(self): i32; }\n"
              "enum Status<T> { Ok = 0, Err = 1 }\n"
              "union Any<T, U> { T, U }\n"
              "implement Status<T> as Value { fn value(self): i32 { return @sizeOf(T) as i32; } }\n"
              "implement Any<T, U> as Value { fn value(self): i32 { return @sizeOf(T) as i32; } }\n"
              "fn main(): i32 {\n"
              "    let s: Status<i32> = Status.Ok;\n"
              "    var a: Any<i32, f64> = Any<i32, f64>{ 42 };\n"
              "    return s.value() + a.value() - @sizeOf(i32) as i32;\n"
              "}\n");
    CHECK(r.ok, "generic enum and union implement blocks bind direct trait calls");
}

void test_dyn_trait_from_generic_enum_and_union() {
    GenericEnumUnionTest t;
    auto r =
        t.run("trait Value { fn value(self): i32; }\n"
              "enum Status<T> { Ok = 0, Err = 1 }\n"
              "union Any<T, U> { T, U }\n"
              "implement Status<T> as Value { fn value(self): i32 { return @sizeOf(T) as i32; } }\n"
              "implement Any<T, U> as Value { fn value(self): i32 { return @sizeOf(T) as i32; } }\n"
              "fn value_of(v: dyn Value): i32 { return v.value(); }\n"
              "fn main(): i32 {\n"
              "    let s: Status<i32> = Status.Ok;\n"
              "    var a: Any<i32, f64> = Any<i32, f64>{ 42 };\n"
              "    return value_of(s) + value_of(a) - @sizeOf(i32) as i32;\n"
              "}\n");
    CHECK(r.ok, "concrete generic enum and union instances form dyn trait receivers");
}

void test_generic_arity_errors() {
    GenericEnumUnionTest bare;
    auto bare_result = bare.run("enum Status<T> { Ok = 0 }\n"
                                "fn main(): i32 {\n"
                                "    let s: Status = Status.Ok;\n"
                                "    return 0;\n"
                                "}\n");
    CHECK(!bare_result.ok, "a generic enum without type arguments is rejected");
    CHECK(bare_result.hasErrorCode(diagnostics::err::GenericArity),
          "the bare generic enum reports E3010");

    GenericEnumUnionTest extra;
    auto extra_result = extra.run("union Any<T, U> { T, U }\n"
                                  "fn main(): i32 {\n"
                                  "    var a: Any<i32, f64, bool> = Any<i32, f64, bool>{ true };\n"
                                  "    return 0;\n"
                                  "}\n");
    CHECK(!extra_result.ok, "a generic union with too many type arguments is rejected");
    CHECK(extra_result.hasErrorCode(diagnostics::err::GenericArity),
          "the wrong union arity reports E3010");
}

void test_self_generic_parameter_rejected() {
    GenericEnumUnionTest t;
    auto r = t.run("struct Pair<T> { value: T }\n"
                   "fn main(): i32 {\n"
                   "    let p: Pair<Self> = Pair<Self>{ value: 1 };\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(!r.ok, "Self<T> is not a valid generic argument");
    CHECK(r.hasErrorCode(diagnostics::err::UndefinedIdent),
          "the invalid Self generic argument reports E2001");
}

void test_non_constant_enum_discriminant_rejected() {
    GenericEnumUnionTest t;
    auto r = t.run("enum Status<T> {\n"
                   "    Bad = @sizeOf(T) as i32,\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let s: Status<i32> = Status.Bad;\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(!r.ok, "enum discriminants must be constant in a generic template");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch) ||
              r.hasErrorCode(diagnostics::err::UndefinedIdent),
          "the non-constant discriminant is rejected at template lowering");
}

void test_union_template_raw_casts_lower() {
    GenericEnumUnionTest t;
    auto r = t.run("union Any<T, U> { T, U }\n"
                   "fn main(): i32 {\n"
                   "    var a: Any<i32, f64> = Any<i32, f64>{ 42 };\n"
                   "    let n: i32 = raw a as i32;\n"
                   "    return n;\n"
                   "}\n");
    CHECK(r.ok, "a generic union raw cast to a member lowers after instantiation");
}

} // namespace

int main() {
    std::printf("enum-union generics tests\n");
    std::printf("==========================\n\n");
    g_test_passed = 0;
    g_test_failed = 0;
    test_inline_generic_enum_method();
    test_inline_generic_union_method();
    test_generic_implement_enum_and_union();
    test_dyn_trait_from_generic_enum_and_union();
    test_generic_arity_errors();
    test_self_generic_parameter_rejected();
    test_non_constant_enum_discriminant_rejected();
    test_union_template_raw_casts_lower();
    std::printf("\nResults: %d passed, %d failed\n", g_test_passed, g_test_failed);
    return g_test_failed > 0 ? 1 : 0;
}
