#pragma once

#include "cli/project-config.hpp"
#include "cli/terminal.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
#include "memory/string-interner.hpp"
#include "session/pipeline-plan.hpp"

namespace zith::cli::commands {
int help(FILE *dest);
}
#include <bitset>
#include <cstdint>
#include <string>

namespace zith {

struct Session;

struct Options {
    memory::DynArray<std::string> inputFiles;
    std::string outputFile;
    memory::DynArray<std::string> includeDirs;
    memory::DynArray<std::string> assetDirs;
    memory::DynArray<std::string> libraryDirs;
    memory::DynArray<std::string> libraries;
    memory::DynArray<std::string> defines;

    enum class Mode : uint8_t { Custom, Debug, Develop, Release, Fast, Small };

    enum class monoformLevel : uint8_t { No, Auto, High, Max };
    enum class Color : uint8_t { Off, Auto, On };
    enum class EmitTarget : uint8_t { None, Ast, Hir, Ir, Asm, Obj, Bin };

    // A target triple belongs to the compilation options, not to the CLI
    // string interner. Compilation sessions have their own interner.
    std::string targetTriple;

    // Sysroot for cross-compilation (passed to the linker as --sysroot)
    std::string sysroot;

    // Resolve C system headers (stdint.h, ...) from the toolchain's default
    // include directories. Cleared by --no-system-includes.
    bool systemIncludes = true;

    // Bit-packed flags (std::bitset<24>):
    //  0-1:  optLevel      (2 bits, values 0-3)
    //  2-3:  debugLevel    (2 bits, values 0-3)
    //  4-5:  color         (2 bits, values 0-2: Off, Auto, On)
    //  6:    strict        (1 bit)
    //  7:    lto           (1 bit)
    //  8:    stripDebug    (1 bit)
    //  9-11: mode          (3 bits, values 0-5: Custom, Debug, Develop, Release, Fast, Small)
    //  12:   interpreted   (1 bit)
    //  13:   verbose       (1 bit)
    //  14:   fmtCheck      (1 bit)
    //  15:   fmtInPlace    (1 bit)
    //  16:   emitTokens    (1 bit)
    //  17:   emitAst       (1 bit)
    //  18:   emitHir       (1 bit)
    //  19:   emitIr        (1 bit)
    //  20:   emitAsm       (1 bit)
    //  21:   printTokens   (1 bit)
    //  22:   cacheStats    (1 bit)
    struct {
        std::bitset<24> bits{};

        static uint8_t extractBits(const std::bitset<24> &b, size_t pos, size_t count) {
            uint8_t val = 0;
            for (size_t i = 0; i < count; ++i)
                if (b.test(pos + i))
                    val |= static_cast<uint8_t>(1 << i);
            return val;
        }
        static void insertBits(std::bitset<24> &b, size_t pos, size_t count, uint8_t val) {
            for (size_t i = 0; i < count; ++i)
                b.set(pos + i, (val >> i) & 1);
        }

        uint8_t optLevel() const {
            return extractBits(bits, 0, 2);
        }
        void optLevel(uint8_t v) {
            insertBits(bits, 0, 2, v);
        }

        uint8_t debugLevel() const {
            return extractBits(bits, 2, 2);
        }
        void debugLevel(uint8_t v) {
            insertBits(bits, 2, 2, v);
        }

        Color color() const {
            return static_cast<Color>(extractBits(bits, 4, 2));
        }
        void color(Color v) {
            insertBits(bits, 4, 2, static_cast<uint8_t>(v));
        }

        bool strict() const {
            return bits.test(6);
        }
        void strict(bool v) {
            bits.set(6, v);
        }

        bool lto() const {
            return bits.test(7);
        }
        void lto(bool v) {
            bits.set(7, v);
        }

        bool stripDebug() const {
            return bits.test(8);
        }
        void stripDebug(bool v) {
            bits.set(8, v);
        }

        Mode mode() const {
            return static_cast<Mode>(extractBits(bits, 9, 3));
        }
        void mode(Mode v) {
            insertBits(bits, 9, 3, static_cast<uint8_t>(v));
        }

        bool interpreted() const {
            return bits.test(12);
        }
        void interpreted(bool v) {
            bits.set(12, v);
        }

        bool verbose() const {
            return bits.test(13);
        }
        void verbose(bool v) {
            bits.set(13, v);
        }

        bool fmtCheck() const {
            return bits.test(14);
        }
        void fmtCheck(bool v) {
            bits.set(14, v);
        }

        bool fmtInPlace() const {
            return bits.test(15);
        }
        void fmtInPlace(bool v) {
            bits.set(15, v);
        }

        bool emitTokens() const {
            return bits.test(16);
        }
        void emitTokens(bool v) {
            bits.set(16, v);
        }

        bool emitAst() const {
            return bits.test(17);
        }
        void emitAst(bool v) {
            bits.set(17, v);
        }

        bool emitHir() const {
            return bits.test(18);
        }
        void emitHir(bool v) {
            bits.set(18, v);
        }

        bool emitIr() const {
            return bits.test(19);
        }
        void emitIr(bool v) {
            bits.set(19, v);
        }

        bool emitAsm() const {
            return bits.test(20);
        }
        void emitAsm(bool v) {
            bits.set(20, v);
        }

        bool printTokens() const {
            return bits.test(21);
        }
        void printTokens(bool v) {
            bits.set(21, v);
        }

        bool cacheStats() const {
            return bits.test(22);
        }
        void cacheStats(bool v) {
            bits.set(22, v);
        }
    } flags;

    // Tracks which mode-dependent fields were explicitly set by CLI
    // so loadFlags() doesn't overwrite them.
    uint8_t cliFields                       = 0;
    static constexpr uint8_t kCliOptLevel   = 1 << 0;
    static constexpr uint8_t kCliDebugLevel = 1 << 1;
    static constexpr uint8_t kCliStripDebug = 1 << 2;
    static constexpr uint8_t kCliLto        = 1 << 3;

    EmitTarget emitTarget      = EmitTarget::None;
    session::Stage targetStage = session::Stage::Cached;

    enum class Command {
        None,
        Build,
        Run,
        Check,
        Execute,
        Test,
        Fmt,
        Docs,
        Repl,
        Create,
        Clean,
        Deps,
        Version,
        Help,
        Completion
    } command = Command::None;
    memory::InternedId subcommandArg;
    std::string subcommandStr; // string copy for command functions

    explicit Options(memory::Arena &allocator)
        : inputFiles(allocator), outputFile(), includeDirs(allocator), assetDirs(allocator),
          libraryDirs(allocator), libraries(allocator), defines(allocator), targetTriple(),
          sysroot(), flags() {}

    memory::StringInterner *stringPool = nullptr;

    void deriveTargetStage();
};

struct Cli {

    Cli()
        : generalAllocator(), opts(generalAllocator), config(generalAllocator),
          stringPool(generalAllocator), out(stdout, false), err(stderr, false) {
        opts.stringPool = &stringPool;
        term::enableVirtual();
    }

    void parseArgs(int argc, char **argv);
    void loadFlags();
    void loadProject();
    int dispatch();

    void requireValue(int i, const char *flag) {
        if (i + 1 >= args.first) {
            err.red("[error]");
            err.red(flag);
            err.red(" requires a value\n");
            cli::commands::help(stderr);
            std::exit(1);
        }
    }

    memory::Arena generalAllocator;
    Options opts;
    ProjectConfig config;
    memory::StringInterner stringPool;
    term::UsagePrinter out;
    term::UsagePrinter err;
    std::pair<int, char **> args;
    int current      = 1;
    Session *session = nullptr;
};

} // namespace zith
