#include "test-common.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

namespace fs = std::filesystem;

struct CliResult {
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

std::string describeResult(const CliResult &result) {
    return "exit code: " + std::to_string(result.exitCode) + "\nstdout:\n" + result.stdoutText +
           "\nstderr:\n" + result.stderrText;
}

struct TestWorkspace {
    fs::path root = fs::temp_directory_path() /
                    ("zith-test-suite-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    TestWorkspace() {
        fs::copy(fs::path(ZITH_TEST_SUITE_DIR), root, fs::copy_options::recursive);

        const auto portableProject = root / "portable-project";
        fs::create_directories(portableProject);
        std::ofstream manifest(portableProject / "ZithProject.toml", std::ios::binary);
        manifest << "[paths]\n"
                    "test_dir = \"tests\"\n";
        CHECK(manifest.good(), "portable backend project manifest is written");

        const auto portableTests = portableProject / "tests";
        fs::create_directories(portableTests);
        std::ofstream smoke(portableTests / "smoke.zithm", std::ios::binary);
        smoke << "#[test]\n"
                 "fn portable_pass() {\n"
                 "    @log(\"passing portable log sentinel\");\n"
                 "    @assert(true);\n"
                 "}\n"
                 "#[test]\n"
                 "fn portable_fail() {\n"
                 "    @log(\"failing portable log sentinel\");\n"
                 "    @assert(false);\n"
                 "}\n";
        CHECK(smoke.good(), "portable backend fixture is written");

        const auto checksProject = root / "checks-project";
        fs::create_directories(checksProject / "tests");
        std::ofstream checksManifest(checksProject / "ZithProject.toml", std::ios::binary);
        checksManifest << "[paths]\n"
                          "test_dir = \"tests\"\n";
        CHECK(checksManifest.good(), "check intrinsic project manifest is written");

        std::ofstream checks(checksProject / "tests" / "checks.zithm", std::ios::binary);
        checks << "from std/test\n"
                  "\n"
                  "#[test]\n"
                  "fn accumulated_checks() {\n"
                  "    @check(true);\n"
                  "    @check(false);\n"
                  "    @check(false);\n"
                  "    @log(\"continued after checks\");\n"
                  "}\n"
                  "\n"
                  "#[test]\n"
                  "fn check_helpers_accumulate() {\n"
                  "    @checkEq(2, 3);\n"
                  "    @checkTrue(false);\n"
                  "    @log(\"continued after helper checks\");\n"
                  "}\n"
                  "\n"
                  "#[test]\n"
                  "fn assert_remains_fail_fast() {\n"
                  "    @assert(false);\n"
                  "    @log(\"after assert must not run\");\n"
                  "    @check(false);\n"
                  "}\n"
                  "\n"
                  "#[test]\n"
                  "fn check_helpers_pass() {\n"
                  "    @log(\"\\x1eZITH-TEST-CHECK-FAIL:checks::check_helpers_pass 1\");\n"
                  "    @checkTrue(true);\n"
                  "    @checkEq(3, 3);\n"
                  "}\n"
                  "\n"
                  "#[test]\n"
                  "fn check_before_assert_reports_both() {\n"
                  "    @check(false);\n"
                  "    @assert(false);\n"
                  "    @log(\"after later assert must not run\");\n"
                  "}\n"
                  "\n"
                  "#[test]\n"
                  "fn check_each_one_of_accumulates() {\n"
                  "    let actuals: [3]i32 = [1, 4, 6];\n"
                  "    let expected: [2]i32 = [1, 3];\n"
                  "    @checkEachOneOf(actuals, expected);\n"
                  "    @log(\"continued after each-one-of checks\");\n"
                  "}\n";
        CHECK(checks.good(), "check intrinsic fixture is written");

        const auto sequenceProject = root / "sequence-project";
        fs::create_directories(sequenceProject / "tests");
        std::ofstream sequenceManifest(sequenceProject / "ZithProject.toml", std::ios::binary);
        sequenceManifest << "[paths]\n"
                            "test_dir = \"tests\"\n";
        CHECK(sequenceManifest.good(), "sequential test project manifest is written");

        std::ofstream assertSequence(sequenceProject / "tests" / "assert-sequence.zithm",
                                     std::ios::binary);
        assertSequence << "from std/test\n"
                          "\n"
                          "#[test]\n"
                          "fn sequence_assert_fails() {\n"
                          "    @assert(false);\n"
                          "}\n"
                          "\n"
                          "#[test]\n"
                          "fn sequence_optional_passes() {\n"
                          "    let present: ?i32 = 42;\n"
                          "    let absent: ?i32 = null;\n"
                          "    @assertSome(present);\n"
                          "    @assertNone(absent);\n"
                          "}\n";
        CHECK(assertSequence.good(), "sequential assert fixture is written");

        std::ofstream oneOfSequence(sequenceProject / "tests" / "one-of-sequence.zithm",
                                    std::ios::binary);
        oneOfSequence << "from std/test\n"
                         "\n"
                         "#[test]\n"
                         "fn sequence_one_of_fails() {\n"
                         "    let missing: i32 = 4;\n"
                         "    let values: [3]i32 = [1, 3, 5];\n"
                         "    @assertOneOf(missing, values);\n"
                         "}\n"
                         "\n"
                         "#[test]\n"
                         "fn sequence_optional_passes() {\n"
                         "    let present: ?i32 = 42;\n"
                         "    let absent: ?i32 = null;\n"
                         "    @assertSome(present);\n"
                         "    @assertNone(absent);\n"
                         "}\n";
        CHECK(oneOfSequence.good(), "sequential one-of fixture is written");

        const auto logsProject = root / "logs-project";
        fs::create_directories(logsProject / "tests");
        std::ofstream logsManifest(logsProject / "ZithProject.toml", std::ios::binary);
        logsManifest << "[paths]\n"
                        "test_dir = \"tests\"\n";
        CHECK(logsManifest.good(), "bounded log project manifest is written");

        std::ofstream logs(logsProject / "tests" / "logs.zithm", std::ios::binary);
        logs << "fn helper_log() {\n"
                "    @log(\"helper log sentinel\");\n"
                "}\n"
                "#[test]\n"
                "fn helper_log_passes() {\n"
                "    helper_log();\n"
                "    @assert(true);\n"
                "}\n"
                "#[test]\n"
                "fn exact_log_capacity() {\n"
                "    @log(\""
             << std::string(4095U, 'x')
             << "\");\n"
                "    @assert(false);\n"
                "}\n"
                "#[test]\n"
                "fn over_log_capacity() {\n"
                "    @log(\""
             << std::string(4096U, 'y')
             << "\");\n"
                "    @assert(false);\n"
                "}\n";
        CHECK(logs.good(), "bounded log fixtures are written");
    }

    ~TestWorkspace() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    CliResult run(const std::string &backend, const std::string &arguments, const std::string &name,
                  fs::path testRoot = {}) const {
        const fs::path stdoutPath = root / (name + ".stdout");
        const fs::path stderrPath = root / (name + ".stderr");
        if (testRoot.empty())
            testRoot = root;
        std::string command = "\"" ZITHC_BINARY "\" --include \"" ZITH_STDLIB_DIR "\" ";
        if (!backend.empty())
            command += backend + " ";
        command += "test " + arguments + " \"" + testRoot.string() + "\" > \"" +
                   stdoutPath.string() + "\" 2> \"" + stderrPath.string() + "\"";

        const int status = std::system(command.c_str());
        int exitCode     = status;
#ifndef _WIN32
        exitCode = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
#endif
        return {exitCode, readFile(stdoutPath), readFile(stderrPath)};
    }

private:
    static std::string readFile(const fs::path &path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
};

CliResult test_backend(const TestWorkspace &workspace, const std::string &backend,
                       const std::string &label) {
    const auto passing =
        workspace.run(backend, "--filter=assertions::assertion_helpers_pass", label + "-pass");
    CHECK(passing.exitCode == 0, (label + " runs passing std/test helpers").c_str());
    CHECK(passing.stdoutText.find("assertion_helpers_pass") != std::string::npos,
          (label + " reports the passing test name").c_str());
    CHECK(passing.stdoutText.find("1 passed; 0 failed; 1 total") != std::string::npos,
          (label + " reports the passing-test summary").c_str());
    CHECK(passing.stdoutText.find("passing test log sentinel") == std::string::npos,
          (label + " hides logs from passing tests by default").c_str());

    const auto verbose = workspace.run(
        backend, "--filter=assertions::assertion_helpers_pass --verbose", label + "-verbose");
    CHECK(verbose.exitCode == 0, (label + " runs passing tests in verbose mode").c_str());
    CHECK(verbose.stdoutText.find("passing test log sentinel") != std::string::npos,
          (label + " shows passing-test logs in verbose mode").c_str());

    const auto custom =
        workspace.run(backend, "--filter=assertions::user_defined_formatable --verbose",
                      label + "-custom-format");
    CHECK(custom.exitCode == 0,
          (label + " runs a user-defined Formatable implementation\n" + describeResult(custom))
              .c_str());
    CHECK(custom.stdoutText.find("custom=ticket:42") != std::string::npos,
          (label + " preserves generic formatting for user-defined Formatable values").c_str());

    const auto all = workspace.run(backend, "", label + "-all");
    CHECK(all.exitCode == 1,
          (label + " returns failure for intentional assertion failures").c_str());
    CHECK(all.stdoutText.find("assert_eq_reports_mismatch") != std::string::npos,
          (label + " reports the assertEq failure").c_str());
    CHECK(all.stdoutText.find("assert_in_reports_missing_value") != std::string::npos,
          (label + " reports the assertIn failure").c_str());
    CHECK(all.stdoutText.find("one_of_helpers_pass") != std::string::npos,
          (label + " reports passing assertOneOf cases").c_str());
    CHECK(all.stdoutText.find("assert_one_of_reports_missing_value") != std::string::npos,
          (label + " reports an assertOneOf failure for a missing value").c_str());
    CHECK(all.stdoutText.find("assert_one_of_rejects_empty_values") != std::string::npos,
          (label + " reports an assertOneOf failure for an empty list").c_str());
    CHECK(all.stdoutText.find("assert_not_in_reports_present_value") != std::string::npos,
          (label + " reports the assertNotIn failure").c_str());
    CHECK(all.stdoutText.find("assert_some_reports_none") != std::string::npos,
          (label + " reports the assertSome failure").c_str());
    CHECK(all.stdoutText.find("assert_none_reports_some") != std::string::npos,
          (label + " reports the assertNone failure").c_str());
    CHECK(all.stdoutText.find("near_helpers_pass") != std::string::npos,
          (label + " reports the passing float assertion test").c_str());
    CHECK(all.stdoutText.find("assert_near_reports_outside_tolerance") != std::string::npos,
          (label + " reports an assertNear failure outside tolerance").c_str());
    CHECK(all.stdoutText.find("assert_near_rejects_negative_tolerance") != std::string::npos,
          (label + " reports an assertNear failure for negative tolerance").c_str());
    CHECK(all.stdoutText.find("fail_reports_message") != std::string::npos,
          (label + " reports the fail helper failure").c_str());
    CHECK(all.stdoutText.find("assertEq failed: actual 5, expected 4") != std::string::npos,
          (label + " formats actual and expected values").c_str());
    CHECK(all.stdoutText.find("assertIn failed: value is not contained") != std::string::npos,
          (label + " explains a missing membership value").c_str());
    CHECK(all.stdoutText.find("assertOneOf failed: value is not in the expected values") !=
              std::string::npos,
          (label + " explains a missing expected value").c_str());
    CHECK(all.stdoutText.find("assertNotIn failed: value is contained") != std::string::npos,
          (label + " explains an unexpected membership value").c_str());
    CHECK(all.stdoutText.find("assertSome failed: value is null") != std::string::npos,
          (label + " explains an absent optional").c_str());
    CHECK(all.stdoutText.find("assertNone failed: value is present") != std::string::npos,
          (label + " explains a present optional").c_str());
    CHECK(all.stdoutText.find("assertNear failed: values are outside tolerance") !=
              std::string::npos,
          (label + " explains an assertNear failure").c_str());
    CHECK(all.stdoutText.find("intentional failure") != std::string::npos,
          (label + " includes the fail message").c_str());
    CHECK(all.stdoutText.find("6 passed; 10 failed; 16 total") != std::string::npos,
          (label + " reports all test results").c_str());
    CHECK(all.stderrText.find("no #[test] functions") != std::string::npos,
          (label + " notes the file with no tests as skipped").c_str());
    CHECK(all.stderrText.find("assertions.zithm:16:") != std::string::npos,
          (label + " reports the failing assert source location").c_str());
    CHECK(all.stderrText.find("floating.zithm:") != std::string::npos,
          (label + " reports the failing float assertion source location").c_str());
    CHECK(all.stderrText.find("collection_optional.zithm:") != std::string::npos,
          (label + " reports the failing collection assertion source location").c_str());
    return all;
}

void test_portable_backend(const TestWorkspace &workspace, const std::string &backend,
                           const std::string &label) {
    const auto testRoot = workspace.root / "portable-project";
    const auto passing =
        workspace.run(backend, "--filter=smoke::portable_pass", label + "-portable-pass", testRoot);
    CHECK(passing.exitCode == 0,
          (label + " runs a backend-compatible passing test\n" + describeResult(passing)).c_str());
    CHECK(passing.stdoutText.find("1 passed; 0 failed; 1 total") != std::string::npos,
          (label + " reports the backend-compatible passing result").c_str());
    CHECK(passing.stdoutText.find("passing portable log sentinel") == std::string::npos,
          (label + " suppresses passing-test logs by default").c_str());

    const auto verbose = workspace.run(backend, "--filter=smoke::portable_pass --verbose",
                                       label + "-portable-verbose", testRoot);
    CHECK(verbose.exitCode == 0,
          (label + " runs backend-compatible tests verbosely\n" + describeResult(verbose)).c_str());
    CHECK(verbose.stdoutText.find("passing portable log sentinel") != std::string::npos,
          (label + " shows passing-test logs in verbose mode").c_str());

    const auto all = workspace.run(backend, "", label + "-portable-all", testRoot);
    CHECK(all.exitCode == 1, (label + " reports a failing backend-compatible test").c_str());
    CHECK(
        all.stdoutText.find("1 passed; 1 failed; 2 total") != std::string::npos,
        (label + " reports both backend-compatible test results\n" + describeResult(all)).c_str());
    CHECK(all.stdoutText.find("failing portable log sentinel") != std::string::npos,
          (label + " shows failed-test logs by default").c_str());
    CHECK(all.stderrText.find("smoke.zithm:9:") != std::string::npos,
          (label + " reports the backend-compatible assert location").c_str());
}

void test_check_backend(const TestWorkspace &workspace, const std::string &backend,
                        const std::string &label) {
    const auto testRoot = workspace.root / "checks-project";
    const auto result   = workspace.run(backend, "", label + "-checks", testRoot);
    CHECK(result.exitCode == 1,
          (label + " reports accumulated check failures\n" + describeResult(result)).c_str());
    CHECK(result.stderrText.find("unknown macro") == std::string::npos,
          (label + " recognizes check intrinsics and std/test macros").c_str());
    CHECK(result.stdoutText.find("1 passed; 5 failed; 6 total") != std::string::npos,
          (label + " counts failed checks as one failed test").c_str());
    CHECK(result.stdoutText.find("continued after checks") != std::string::npos,
          (label + " continues after direct check failures").c_str());
    CHECK(result.stdoutText.find("continued after helper checks") != std::string::npos,
          (label + " continues after check helper failures").c_str());
    CHECK(result.stdoutText.find("continued after each-one-of checks") != std::string::npos,
          (label + " continues after each-one-of failures").c_str());
    CHECK(result.stdoutText.find("after assert must not run") == std::string::npos,
          (label + " keeps assert fail-fast behavior").c_str());
    CHECK(result.stdoutText.find("after later assert must not run") == std::string::npos,
          (label + " stops the test after an assert following a check").c_str());
    CHECK(result.stdoutText.find("ZITH-TEST-CHECK-FAIL") == std::string::npos,
          (label + " removes internal check records from test output").c_str());
    CHECK(result.stdoutText.find("checkEq failed: actual 2, expected 3") != std::string::npos,
          (label + " preserves checkEq diagnostics").c_str());
    CHECK(result.stdoutText.find("checkTrue failed") != std::string::npos,
          (label + " preserves checkTrue diagnostics").c_str());
    CHECK(result.stdoutText.find(
              "checkEachOneOf failed at index 1: actual 4; expected one of:") !=
              std::string::npos,
          (label + " reports the first mismatching value with its index and allowed values")
              .c_str());
    CHECK(result.stdoutText.find(
              "checkEachOneOf failed at index 2: actual 6; expected one of:") !=
              std::string::npos,
          (label + " accumulates later mismatches with their context").c_str());
    CHECK(result.stdoutText.find("  allowed value: 1") != std::string::npos &&
              result.stdoutText.find("  allowed value: 3") != std::string::npos,
          (label + " lists the allowed values for each mismatch").c_str());
    CHECK(result.stderrText.find("checks.zithm:6:") != std::string::npos,
          (label + " reports the first failed check location").c_str());
    CHECK(result.stderrText.find("checks.zithm:7:") != std::string::npos,
          (label + " reports the second failed check location").c_str());
    CHECK(result.stderrText.find("checks.zithm:13:") != std::string::npos,
          (label + " reports the failed checkEq location").c_str());
    CHECK(result.stderrText.find("checks.zithm:14:") != std::string::npos,
          (label + " reports the failed checkTrue location").c_str());
    CHECK(result.stderrText.find("checks.zithm:20:") != std::string::npos,
          (label + " preserves the assert failure location").c_str());
    CHECK(result.stderrText.find("checks.zithm:34:") != std::string::npos,
          (label + " reports a check before a later assert").c_str());
    CHECK(result.stderrText.find("checks.zithm:35:") != std::string::npos,
          (label + " reports the assert after a failed check").c_str());
    const auto earlierCheck = result.stderrText.find("checks.zithm:34:");
    const auto laterAssert  = result.stderrText.find("checks.zithm:35:");
    CHECK(earlierCheck != std::string::npos && laterAssert != std::string::npos &&
              earlierCheck < laterAssert,
          (label + " reports failures in execution order").c_str());
}

void test_interpreted_check_backend(const TestWorkspace &workspace) {
    const auto testRoot = workspace.root / "checks-project";
    const auto result   = workspace.run("--interpreted", "--filter=checks::accumulated_checks",
                                        "hir-checks", testRoot);
    CHECK(result.exitCode == 1,
          ("HIR interpreter runs accumulated checks\n" + describeResult(result)).c_str());
    CHECK(result.stderrText.find("unknown macro") == std::string::npos,
          "HIR interpreter recognizes the check intrinsic");
    CHECK(result.stdoutText.find("continued after checks") != std::string::npos,
          "HIR interpreter continues after a failed check");
    CHECK(result.stderrText.find("checks.zithm:6:") != std::string::npos,
          "HIR interpreter reports the failed check location");
}

void test_vm_sequential_test_state(const TestWorkspace &workspace) {
    const auto testRoot = workspace.root / "sequence-project";
    const auto result =
        workspace.run("--virtual-machine", "--filter=sequence", "vm-sequence", testRoot);
    CHECK(result.exitCode == 1,
          ("VM isolates sequential test results\n" + describeResult(result)).c_str());
    CHECK(
        result.stdoutText.find("2 passed; 2 failed; 4 total") != std::string::npos,
        ("VM preserves passing tests after failed assertions\n" + describeResult(result)).c_str());
    CHECK(result.stdoutText.find("assert-sequence::sequence_optional_passes") != std::string::npos,
          "VM reports the optional test after a failed assert");
    CHECK(result.stdoutText.find("one-of-sequence::sequence_optional_passes") != std::string::npos,
          "VM reports the optional test after a failed assertOneOf");
}

void test_bounded_logs(const TestWorkspace &workspace, const std::string &backend,
                       const std::string &label) {
    const auto testRoot = workspace.root / "logs-project";
    const auto helper =
        workspace.run(backend, "--filter=logs::helper_log_passes", label + "-helper-log", testRoot);
    CHECK(helper.exitCode == 0, (label + " runs a passing test with a helper log").c_str());
    CHECK(helper.stdoutText.find("helper log sentinel") == std::string::npos,
          (label + " attributes helper logs to the active passing test").c_str());

    const auto helperVerbose = workspace.run(backend, "--filter=logs::helper_log_passes --verbose",
                                             label + "-helper-log-verbose", testRoot);
    CHECK(helperVerbose.stdoutText.find("helper log sentinel") != std::string::npos,
          (label + " shows helper logs in verbose mode").c_str());

    const auto exact = workspace.run(backend, "--filter=logs::exact_log_capacity --verbose",
                                     label + "-exact-log", testRoot);
    CHECK(exact.exitCode == 1, (label + " reports the exact-capacity test failure").c_str());
    CHECK(exact.stdoutText.find(std::string(4095U, 'x')) != std::string::npos,
          (label + " retains the exact-capacity log body").c_str());
    CHECK(exact.stdoutText.find(std::string(4096U, 'x')) == std::string::npos,
          (label + " excludes bytes beyond the exact-capacity log").c_str());

    const auto over = workspace.run(backend, "--filter=logs::over_log_capacity --verbose",
                                    label + "-over-log", testRoot);
    CHECK(over.exitCode == 1, (label + " reports the over-capacity test failure").c_str());
    CHECK(over.stdoutText.find(std::string(4096U, 'y')) != std::string::npos,
          (label + " retains exactly the configured log capacity").c_str());
    CHECK(over.stdoutText.find(std::string(4097U, 'y')) == std::string::npos,
          (label + " truncates log bytes beyond the configured capacity").c_str());
}

void test_zith_test_suite() {
#ifdef ZITHC_BINARY
    TestWorkspace workspace;
#ifdef ZITH_HAS_VM
    const auto nativeReport = test_backend(workspace, "", "default backend");
#else
    test_backend(workspace, "", "default backend");
#endif
    test_bounded_logs(workspace, "", "default backend");
    test_check_backend(workspace, "", "default backend");
    test_vm_sequential_test_state(workspace);
    // Rich std/test formatting is not yet supported by the HIR interpreter.
    test_bounded_logs(workspace, "--interpreted", "HIR interpreter");
    test_interpreted_check_backend(workspace);
    test_portable_backend(workspace, "--interpreted", "HIR interpreter");
#ifdef ZITH_HAS_VM
    const auto vmReport = test_backend(workspace, "--virtual-machine", "VM v2");
    CHECK(nativeReport.stdoutText == vmReport.stdoutText,
          "native and VM v2 produce identical test report output");
    CHECK(nativeReport.stderrText == vmReport.stderrText,
          "native and VM v2 produce identical failure locations");
    test_bounded_logs(workspace, "--virtual-machine", "VM v2");
    test_check_backend(workspace, "--virtual-machine", "VM v2");
    test_portable_backend(workspace, "--virtual-machine", "VM v2");
#endif
#else
    CHECK(true, "zithc integration suite is not built without the CLI target");
#endif
}

} // namespace

TEST_MAIN(zith_test_suite)
