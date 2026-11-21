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
        : opts(arena),
          root(std::filesystem::temp_directory_path() / "zith-nominal-type-debt-tests") {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        opts.targetStage = session::Stage::TypeChecked;
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
    };

    Result run(std::string_view input) {
        const auto path = root / "main.zithm";
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
        const bool ok = session.runTo(session::Stage::TypeChecked);

        std::vector<Result::Diag> copied;
        for (const auto &diag : session.diags().all()) {
            copied.push_back({static_cast<diagnostics::ErrCode>(diag.code), diag.message});
            if (diag.severity == diagnostics::Severity::Error) {
                std::printf("    [NominalTypeDebt] Code: %u, Message: %s\n", diag.code,
                            diag.message.c_str());
            }
        }
        return {ok && session.diags().errorCount() == 0, std::move(copied)};
    }
};

void test_construction_and_extraction() {
    SessionRunner t;
    auto r = t.run("type UserId = i32\n"
                   "fn makeId(n: i32): UserId { return n as UserId; }\n"
                   "fn readId(id: UserId): i32 { return id as i32; }\n"
                   "fn main(): i32 {\n"
                   "    let id: UserId = makeId(7);\n"
                   "    return readId(id);\n"
                   "}\n");
    CHECK(r.ok, "nominal wrapper is explicitly constructed and extracted with as");
}

void test_nominal_extraction_requires_explicit_cast() {
    SessionRunner underlying;
    auto underlying_result = underlying.run("type UserId = i32\n"
                                            "fn main(): i32 {\n"
                                            "    let id: UserId = 1 as UserId;\n"
                                            "    return id;\n"
                                            "}\n");
    CHECK(!underlying_result.ok,
          "a nominal value is not implicitly convertible to its underlying type");
    CHECK(underlying_result.hasErrorCode(diagnostics::err::TypeMismatch),
          "implicit nominal withdrawal reports E3001");
}

void test_distinct_nominal_names_are_not_casts() {
    SessionRunner t;
    auto r = t.run("type UserId = i32\n"
                   "type SessionId = i32\n"
                   "fn main(): i32 {\n"
                   "    let id: UserId = 1 as UserId;\n"
                   "    let other: SessionId = id as SessionId;\n"
                   "    return other as i32;\n"
                   "}\n");
    CHECK(!r.ok, "casts between different nominal names are rejected");
    CHECK(r.hasErrorCode(diagnostics::err::InvalidCast),
          "different nominal names report E3003 on direct as");
}

void test_alias_remains_transparent() {
    SessionRunner t;
    auto r = t.run("alias Number = i32\n"
                   "fn expectInt(n: i32): i32 { return n; }\n"
                   "fn main(): i32 {\n"
                   "    let n: Number = 3;\n"
                   "    return expectInt(n);\n"
                   "}\n");
    CHECK(r.ok, "an alias stays transparent and unifies with its target type");
}

void test_nominal_type_debt() {
    test_construction_and_extraction();
    test_nominal_extraction_requires_explicit_cast();
    test_distinct_nominal_names_are_not_casts();
    test_alias_remains_transparent();
}

} // namespace

TEST_MAIN(nominal_type_debt)
