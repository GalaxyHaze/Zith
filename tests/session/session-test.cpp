#include "session/dispatch.hpp"
#include "session/session.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using toolkit::session::CompilationSession;
using toolkit::session::PipelinePlan;
using toolkit::session::Stage;

namespace {

std::vector<Stage> g_calls;

bool check(bool ok, std::string_view message) {
    if (!ok)
        std::fprintf(stderr, "FAIL: %.*s\n", static_cast<int>(message.size()), message.data());
    return ok;
}

void record(Stage stage) {
    g_calls.push_back(stage);
}

} // namespace

template <>
common::memory::Result<toolkit::session::SourceResult>
toolkit::session::dispatch<Stage::Source>(CompilationSession &) {
    record(Stage::Source);
    return {};
}

template <>
common::memory::Result<toolkit::session::LexedResult>
toolkit::session::dispatch<Stage::Lexed>(CompilationSession &session) {
    if (session.context().filePath.empty())
        return common::memory::Error{"missing file"};
    record(Stage::Lexed);
    std::vector<generated_lexer::Token> tokens{
        generated_lexer::Token{
            common::memory::Span{0, 0},
            generated_lexer::TokenKind::End,
        },
    };
    return generated_lexer::TokenStream{std::move(tokens), session.context().interner};
}

template <>
common::memory::Result<toolkit::session::ParsedResult>
toolkit::session::dispatch<Stage::Parsed>(CompilationSession &session) {
    if (!session.hasStageResult<Stage::Lexed>()) {
        return common::memory::Error{"parser ran without a Lexed result"};
    }
    record(Stage::Parsed);
    return sample::ParseOutput{};
}

template <>
common::memory::Result<toolkit::session::ScannedResult>
toolkit::session::dispatch<Stage::Scanned>(CompilationSession &session) {
    if (!session.hasStageResult<Stage::Lexed>()) {
        return common::memory::Error{"scanner ran without a Lexed result"};
    }
    const auto &tokens = session.stageResult<Stage::Lexed>().value();
    if (tokens.empty()) {
        return common::memory::Error{"scanner rejected empty token stream"};
    }
    record(Stage::Scanned);
    return {};
}

template <>
common::memory::Result<toolkit::session::ImportedResult>
toolkit::session::dispatch<Stage::Imported>(CompilationSession &) {
    record(Stage::Imported);
    return {};
}

template <>
common::memory::Result<toolkit::session::ResolvedResult>
toolkit::session::dispatch<Stage::Resolved>(CompilationSession &) {
    record(Stage::Resolved);
    return {};
}

template <>
common::memory::Result<toolkit::session::TypeCheckedResult>
toolkit::session::dispatch<Stage::TypeChecked>(CompilationSession &) {
    record(Stage::TypeChecked);
    return {};
}

template <>
common::memory::Result<toolkit::session::SolvedResult>
toolkit::session::dispatch<Stage::Solved>(CompilationSession &) {
    record(Stage::Solved);
    return {};
}

template <>
common::memory::Result<toolkit::session::NraResolvedResult>
toolkit::session::dispatch<Stage::NraResolved>(CompilationSession &) {
    record(Stage::NraResolved);
    return {};
}

template <>
common::memory::Result<toolkit::session::HirLoweredResult>
toolkit::session::dispatch<Stage::HirLowered>(CompilationSession &) {
    record(Stage::HirLowered);
    return {};
}

template <>
common::memory::Result<toolkit::session::CodegenReadyResult>
toolkit::session::dispatch<Stage::CodegenReady>(CompilationSession &) {
    record(Stage::CodegenReady);
    return {};
}

template <>
common::memory::Result<toolkit::session::CachedResult>
toolkit::session::dispatch<Stage::Cached>(CompilationSession &) {
    record(Stage::Cached);
    return {};
}

int main() {
    bool ok = true;

    Stage stages[] = {
        Stage::Source,
        Stage::Lexed,
        Stage::Parsed,
        Stage::Scanned,
        Stage::Imported,
        Stage::Resolved,
        Stage::TypeChecked,
        Stage::Solved,
        Stage::NraResolved,
        Stage::HirLowered,
        Stage::CodegenReady,
        Stage::Cached,
    };
    constexpr size_t stageCount = 12;

    PipelinePlan plan;
    ok &= check(plan.current == Stage::Source, "default current stage is Source");
    ok &= check(plan.target == Stage::Cached, "default target stage is Cached");
    ok &= check(!plan.shouldStop(), "new plan is not stopped before Cached");
    ok &= check(plan.advance(), "advance moves from Source");
    ok &= check(plan.current == Stage::Lexed, "new plan advances to Lexed");

    plan.current = Stage::Source;
    plan.target = Stage::Resolved;
    ok &= check(!plan.shouldStop(), "plan should not stop before target");
    ok &= check(plan.advance(), "advance moves toward target");
    ok &= check(plan.current == Stage::Lexed, "advance moves to Lexed");
    plan.target = Stage::Lexed;
    ok &= check(plan.shouldStop(), "plan stops at target inclusive");
    ok &= check(!plan.advance(), "advance stops at target inclusive");
    ok &= check(plan.current == Stage::Lexed, "advance leaves current at target");

    toolkit::session::ZithSessionContext context;
    CompilationSession session(context);
    ok &= check(&session.context() == &context, "session keeps the injected context");
    ok &= check(context.filePath.empty(), "context starts with no file path");
    ok &= check(context.projectRoot.empty(), "context starts with no project root");
    ok &= check(context.fileId == 0, "context starts at file id zero");
    ok &= check(context.arena.allocatedBytes() > 0, "context arena is alive");
    ok &= check(context.interner.intern("hello") == 0, "context interner interns strings");
    ok &= check(context.interner.lookup(0) == "hello", "context interner looks up strings");
    ok &= check(context.sourceMap.exists(0) == false, "context source map starts empty");
    context.filePath = "input.zith";
    context.projectRoot = "project";
    ok &= check(context.filePath == "input.zith", "context stores the file path");
    ok &= check(context.projectRoot == "project", "context stores the project root");

    g_calls.clear();
    const auto result = session.runTo(Stage::Cached);
    ok &= check(static_cast<bool>(result), "runTo Cached succeeds");
    ok &= check(result.value() == Stage::Cached, "runTo Cached returns Cached");
    ok &= check(g_calls.size() == stageCount, "runTo Cached invokes every stage");
    bool ordered = g_calls.size() == stageCount;
    for (size_t i = 0; i < stageCount && ordered; ++i)
        ordered = g_calls[i] == stages[i];
    ok &= check(ordered, "runTo Cached invokes stages in rule order");
    ok &= check(!session.hasErrors(), "stub pipeline has no errors");
    ok &= check(session.plan.current == Stage::Cached, "runTo leaves current at target");
    ok &= check(session.hasStageResult<Stage::Source>(), "void stage result is stored");
    ok &= check(session.hasStageResult<Stage::Lexed>(), "non-void stage result is stored");
    ok &= check(
        session.stageResult<Stage::Source>().isOk(),
        "void stage result accessor returns success"
    );
    ok &= check(
        session.stageResult<Stage::Lexed>().isOk(),
        "Lexed stage result accessor returns success"
    );
    ok &= check(
        session.stageResult<Stage::Lexed>().value().empty() == false,
        "Scanned stage sees the token stream produced by Lexed"
    );
    ok &= check(
        session.hasStageResult<Stage::Parsed>(),
        "Parsed stage result is stored"
    );
    ok &= check(
        session.stageResult<Stage::Parsed>().isOk(),
        "Parsed stage result accessor returns success"
    );

    toolkit::session::ZithSessionContext failedContext;
    CompilationSession failed(failedContext);
    g_calls.clear();
    const auto failedResult = failed.runTo(Stage::Resolved);
    ok &= check(!failedResult, "runTo with no file path fails");
    ok &= check(
        failedResult.error().stage == Stage::Lexed,
        "runTo error identifies failing stage"
    );
    ok &= check(
        failed.hasStageResult<Stage::Source>(),
        "results before the failing stage are stored"
    );
    ok &= check(
        !failed.hasStageResult<Stage::Lexed>(),
        "failing stage result is absent"
    );

    g_calls.clear();
    const auto direct = dispatch<Stage::Lexed>(failed);
    ok &= check(!direct, "dispatch<Lexed> fails without a file");

    g_calls.clear();

    failed.context().filePath = "input.zith";
    const auto resumed = failed.resume();
    ok &= check(static_cast<bool>(resumed), "resume continues after fixing the failure");
    ok &= check(resumed.value() == Stage::Resolved, "resume reaches the original target");

    g_calls.clear();
    const auto fullRun = failed.run();
    ok &= check(static_cast<bool>(fullRun), "run() reaches the final cached stage");
    ok &= check(fullRun.value() == Stage::Cached, "run() returns the final stage");

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
