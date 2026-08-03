// test-telemetry.cpp - Stage timing and arena memory usage tests

#include "cli/options.hpp"
#include "memory/arena.hpp"
#include "session/compilation-session.hpp"
#include "session/pipeline-plan.hpp"
#include "test-common.hpp"

#include <string>
#include <unordered_map>

using namespace zith;
using namespace zith::session;

// Thin wrapper that owns the arena + options and exposes a session per run.
struct TelemetryHarness {
    memory::Arena arena;
    Options opts;

    TelemetryHarness() : opts(arena) {}

    // Run the session to `target` with the given source; returns timing map and usage.
    struct RunResult {
        std::unordered_map<std::string, double> durations;
        ArenaMemoryUsage usage;
        bool hasErrors;
    };

    RunResult run(Stage target, const char *source) {
        opts.targetStage = target;
        CompilationSession sess(opts, "test.zith");
        sess.setBuffered(true);
        sess.setContent(source);
        sess.run();
        return {sess.getStageDurationsMs(), sess.getArenaMemoryUsage(), sess.hasErrors()};
    }
};

// ── Stage duration tests ──────────────────────────────────────────

static void test_stage_durations_keys() {
    TelemetryHarness h;
    auto r = h.run(Stage::Scanned, "fn hello() { }");

    static const char *expected_keys[] = {
        "lex", "scan", "import", "resolve", "sema", "lower", "solve", "nra", "codegen", "cache",
    };
    for (auto *k : expected_keys) {
        bool found      = r.durations.count(k) > 0;
        std::string msg = std::string("key '") + k + "' in getStageDurationsMs()";
        CHECK(found, msg.c_str());
    }
    CHECK(r.durations.size() == 10, "exactly 10 stage entries");
}

static void test_executed_stages_nonnegative() {
    TelemetryHarness h;
    auto r = h.run(Stage::Scanned, "fn hello() { }");
    CHECK(r.durations["lex"] >= 0.0, "lex duration >= 0");
    CHECK(r.durations["scan"] >= 0.0, "scan duration >= 0");
}

static void test_non_executed_stages_zero() {
    TelemetryHarness h;
    auto r = h.run(Stage::Scanned, "fn hello() { }"); // stops before codegen/cache
    CHECK(r.durations["codegen"] == 0.0, "codegen duration is 0 when not executed");
    CHECK(r.durations["cache"] == 0.0, "cache duration is 0 when not executed");
}

static void test_scan_with_errors_has_timings() {
    TelemetryHarness h;
    auto r = h.run(Stage::Scanned, "@@@ fn good() { }");
    CHECK(r.durations["scan"] >= 0.0, "scan duration set even with errors");
    CHECK(r.hasErrors, "session reports errors from bad token");
}

// ── Arena memory usage tests ──────────────────────────────────────

static void test_arena_usage_fields_accessible() {
    TelemetryHarness h;
    auto r = h.run(Stage::Scanned, "fn a() { } fn b() { }");
    // All fields are size_t; just verify they are reachable without crashing.
    CHECK(r.usage.scratchAllocatedBytes >= 0, "scratch field accessible");
    CHECK(r.usage.symbolAllocatedBytes >= 0, "ast field accessible");
    CHECK(r.usage.symbolAllocatedBytes >= 0, "symbol field accessible");
    CHECK(r.usage.typeAllocatedBytes >= 0, "type field accessible");
    CHECK(r.usage.hirAllocatedBytes >= 0, "hir field accessible");
}

static void test_ast_arena_grows_with_input() {
    TelemetryHarness h_empty;
    auto r_empty = h_empty.run(Stage::Scanned, "");

    TelemetryHarness h_full;
    auto r_full = h_full.run(Stage::Scanned,
                             "struct Foo { x: Int, y: Int, } fn bar(a: Int, b: Int): Int { a }");

    CHECK(r_full.usage.symbolAllocatedBytes > 0, "AST arena has allocations after non-empty parse");
    // Allocated bytes should be >= the empty session (may equal if both fit in 1 block)
    CHECK(r_full.usage.symbolAllocatedBytes >= r_empty.usage.symbolAllocatedBytes,
          "AST arena bytes non-decreasing with more input");
}

// ── Arena unit tests ──────────────────────────────────────────────

static void test_arena_allocated_bytes_nonzero() {
    memory::Arena arena;
    CHECK(arena.allocatedBytes() > 0, "fresh arena has at least one block");
}

static void test_arena_used_bytes_increases() {
    memory::Arena arena;
    size_t used0 = arena.usedBytes();
    arena.alloc(256);
    size_t used1 = arena.usedBytes();
    CHECK(used1 > used0, "usedBytes increases after alloc");
}

static void test_arena_allocated_gte_used() {
    memory::Arena arena;
    arena.alloc(512);
    CHECK(arena.allocatedBytes() >= arena.usedBytes(), "allocatedBytes >= usedBytes after alloc");
}

// ── Runner ───────────────────────────────────────────────────────

static void test_telemetry() {
    test_stage_durations_keys();
    test_executed_stages_nonnegative();
    test_non_executed_stages_zero();
    test_scan_with_errors_has_timings();

    test_arena_usage_fields_accessible();
    test_ast_arena_grows_with_input();

    test_arena_allocated_bytes_nonzero();
    test_arena_used_bytes_increases();
    test_arena_allocated_gte_used();
}

TEST_MAIN(telemetry)
