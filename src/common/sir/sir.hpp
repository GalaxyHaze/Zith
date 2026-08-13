#pragma once

#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"
#include "common/memory/result.hpp"
#include "common/memory/string-interner.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

namespace toolkit::sir {

using common::memory::Arena;
using common::memory::DynArray;
using common::memory::InternedId;
using common::memory::Result;
using common::memory::StringInterner;

enum class TypeKind : uint8_t {
    Void,
    I1,
    I8,
    I16,
    I32,
    I64,
    F32,
    F64,
    UserDefined,
};

struct Type {
    TypeKind kind = TypeKind::Void;
    InternedId nameId = 0;

    constexpr Type() noexcept = default;
    constexpr Type(TypeKind kind_) noexcept : kind(kind_) {}
    constexpr Type(TypeKind kind_, InternedId nameId_) noexcept : kind(kind_), nameId(nameId_) {}

    [[nodiscard]] constexpr bool isVoid() const noexcept {
        return kind == TypeKind::Void;
    }

    [[nodiscard]] constexpr bool isInteger() const noexcept {
        return kind >= TypeKind::I1 && kind <= TypeKind::I64;
    }

    [[nodiscard]] constexpr bool isFloat() const noexcept {
        return kind == TypeKind::F32 || kind == TypeKind::F64;
    }

    [[nodiscard]] constexpr bool isNumeric() const noexcept {
        return isInteger() || isFloat();
    }

    friend constexpr bool operator==(Type left, Type right) noexcept {
        return left.kind == right.kind && left.nameId == right.nameId;
    }
};

struct Primitives {
    static constexpr Type voidT = Type(TypeKind::Void);
    static constexpr Type i1   = Type(TypeKind::I1);
    static constexpr Type i8   = Type(TypeKind::I8);
    static constexpr Type i16  = Type(TypeKind::I16);
    static constexpr Type i32  = Type(TypeKind::I32);
    static constexpr Type i64  = Type(TypeKind::I64);
    static constexpr Type f32  = Type(TypeKind::F32);
    static constexpr Type f64  = Type(TypeKind::F64);
};

enum class Opcode : uint8_t {
    Constant,
    Param,
    Add,
    Sub,
    Mul,
    Store,
    Return,
};

enum class Mutability : uint8_t {
    Mutable,
    Immutable,
};

struct Value;
struct Variable;
struct ScopeData;
struct Function;
struct Module;

struct Variable {
    Arena *arena = nullptr;
    StringInterner *interner = nullptr;
    InternedId name = 0;
    Type type;
    Mutability mutability = Mutability::Mutable;
    Value *initializer = nullptr;
    Function *function = nullptr;
    ScopeData *owner = nullptr;

    template <Type Ty>
    [[nodiscard]] Variable &makeConstant(int64_t value);

    [[nodiscard]] Variable &makeConstant(Type type, int64_t value);
    [[nodiscard]] Variable &makeConstantFloat(Type type, double value);
    [[nodiscard]] Variable &storeConstant(int64_t value);
    [[nodiscard]] Variable &setImmutable() noexcept {
        mutability = Mutability::Immutable;
        return *this;
    }

    [[nodiscard]] std::string_view nameView() const noexcept;
    [[nodiscard]] bool isImmutable() const noexcept {
        return mutability == Mutability::Immutable;
    }
};

struct Value {
    Opcode opcode = Opcode::Constant;
    Type type;
    Function *function = nullptr;
    ScopeData *owner = nullptr;
    Variable *variable = nullptr;
    Value *left = nullptr;
    Value *right = nullptr;

    union {
        int64_t intValue = 0;
        double floatValue;
    };
};

struct Operand {
    Value *value = nullptr;
    Variable *variable = nullptr;
    int64_t intLiteral = 0;
    double floatLiteral = 0.0;
    bool floatLiteralOnly = false;
    bool hasLiteral = false;
    bool hasValue = false;
    bool hasVariable = false;

    Operand() noexcept = default;
    Operand(Value &value_) noexcept : value(&value_), hasValue(true) {}
    Operand(Variable &variable_) noexcept : variable(&variable_), hasVariable(true) {}
    template <std::integral T>
    Operand(T value) noexcept : intLiteral(static_cast<int64_t>(value)), hasLiteral(true) {}

    Operand(double value) noexcept
        : floatLiteral(value), floatLiteralOnly(true), hasLiteral(true) {}
};

struct Instruction {
    Opcode opcode = Opcode::Return;
    Type type;
    Function *function = nullptr;
    ScopeData *owner = nullptr;
    Value *value = nullptr;
    Variable *variable = nullptr;
};

struct ArgsDecl {
    static constexpr size_t maxArgs = 8;

    std::array<Type, maxArgs> types{};
    size_t count = 0;
    uint32_t signatureId = 2166136261u;

    ArgsDecl() noexcept = default;

    ArgsDecl(std::initializer_list<Type> initial) {
        count = initial.size() > maxArgs ? maxArgs : initial.size();
        size_t index = 0;
        for (Type type : initial) {
            if (index >= count)
                break;
            types[index++] = type;
        }
        recomputeSignature();
    }

    [[nodiscard]] Type typeAt(size_t index) const noexcept {
        return index < count ? types[index] : Primitives::voidT;
    }

    [[nodiscard]] size_t size() const noexcept {
        return count;
    }

private:
    void recomputeSignature() noexcept {
        signatureId = 2166136261u;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t kind = static_cast<uint32_t>(types[i].kind);
            signatureId = (signatureId ^ kind) * 16777619u;
            signatureId = (signatureId ^ types[i].nameId) * 16777619u;
        }
    }
};

struct CallArgs {
    static constexpr size_t maxArgs = 8;

    std::array<Value *, maxArgs> values{};
    size_t count = 0;
    uint32_t signatureId = 0;

    [[nodiscard]] size_t size() const noexcept {
        return count;
    }

    [[nodiscard]] Value &at(size_t index) const noexcept {
        return *values[index];
    }
};

struct ScopeData {
    Arena *arena = nullptr;
    Function *function = nullptr;
    ScopeData *parent = nullptr;

    [[nodiscard]] Variable &declVar(std::string_view name, Type type);
    [[nodiscard]] Value &constInt64(int64_t value, Type type);
    [[nodiscard]] Value &constFloat64(double value, Type type);
    [[nodiscard]] Value &add(Operand left, Operand right, Type resultType = {});
    [[nodiscard]] Value &sub(Operand left, Operand right, Type resultType = {});
    [[nodiscard]] Value &mul(Operand left, Operand right, Type resultType = {});
    void store(Variable &variable, Operand value);
    void ret(Operand value);
    void retVoid();
};

class Scope {
public:
    Scope() noexcept = default;
    explicit Scope(ScopeData *data_) noexcept : data_(data_) {}

    [[nodiscard]] ScopeData &get() const {
        if (!data_)
            failNull();
        return *data_;
    }

    [[nodiscard]] ScopeData *getPtr() const noexcept {
        return data_;
    }

    [[nodiscard]] ScopeData &operator*() const {
        return get();
    }

    [[nodiscard]] ScopeData *operator->() const {
        return getPtr();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return data_ != nullptr;
    }

    [[nodiscard]] Variable &declVar(std::string_view name, Type type) {
        return get().declVar(name, type);
    }

    [[nodiscard]] Value &constInt64(int64_t value, Type type) {
        return get().constInt64(value, type);
    }

    [[nodiscard]] Value &constFloat64(double value, Type type) {
        return get().constFloat64(value, type);
    }

    [[nodiscard]] Value &add(Operand left, Operand right, Type resultType = {}) {
        return get().add(left, right, resultType);
    }

    [[nodiscard]] Value &sub(Operand left, Operand right, Type resultType = {}) {
        return get().sub(left, right, resultType);
    }

    [[nodiscard]] Value &mul(Operand left, Operand right, Type resultType = {}) {
        return get().mul(left, right, resultType);
    }

    void store(Variable &variable, Operand value) {
        get().store(variable, value);
    }

    void ret(Operand value) {
        get().ret(value);
    }

    void retVoid() {
        get().retVoid();
    }

private:
    [[noreturn]] static void failNull();

    ScopeData *data_ = nullptr;
};

struct Function {
    Arena *arena = nullptr;
    StringInterner *interner = nullptr;
    InternedId name = 0;
    Type returnType;

    DynArray<Variable *> params;
    DynArray<Value *> paramValues;
    DynArray<ScopeData *> scopes;
    DynArray<Variable *> variables;
    DynArray<Value *> values;
    DynArray<Instruction *> instructions;
    ArgsDecl argsDecl;
    ScopeData *baseScope = nullptr;
    ScopeData *currentScope = nullptr;

    Function(Arena &arena, StringInterner &interner_, std::string_view name_, Type returnType_);

    [[nodiscard]] std::string_view nameView() const noexcept;
    [[nodiscard]] Scope pushScope();

    [[nodiscard]] Value &param(size_t index);
    [[nodiscard]] Value &constInt64(int64_t value, Type type);
    [[nodiscard]] Value &constFloat64(double value, Type type);
    [[nodiscard]] Value &add(Operand left, Operand right, Type resultType = {});
    [[nodiscard]] Value &sub(Operand left, Operand right, Type resultType = {});
    [[nodiscard]] Value &mul(Operand left, Operand right, Type resultType = {});
    void store(Variable &variable, Operand value);
    void ret(Operand value);
    void retVoid();

    [[nodiscard]] CallArgs makeArgs(Operand first);
    [[nodiscard]] CallArgs makeArgs(Operand first, Operand second);
    [[nodiscard]] CallArgs makeArgs(Operand first, Operand second, Operand third);

    [[nodiscard]] ScopeData &current();
};

struct Module {
    Arena *arena = nullptr;
    StringInterner *interner = nullptr;
    InternedId name = 0;
    DynArray<Function *> functions;

    Module(Arena &arena, StringInterner &interner_, std::string_view name_)
        : arena(&arena), interner(&interner_), name(interner_.intern(name_)), functions(arena) {}

    [[nodiscard]] std::string_view nameView() const noexcept {
        return interner->lookup(name);
    }

    [[nodiscard]] Function &declareFn(std::string_view name);
    [[nodiscard]] Function &declareFn(std::string_view name, Type returnType, ArgsDecl args);
};

struct SirBuilder {
    Arena &arena;
    StringInterner interner;

    explicit SirBuilder(Arena &arena_) : arena(arena_), interner(arena_) {}

    [[nodiscard]] Module &createModule(std::string_view name) {
        auto *module = arena.make<Module>(arena, interner, name);
        if (!module)
            failAllocation("module");
        return *module;
    }

private:
    [[noreturn]] static void failAllocation(std::string_view what);
};

[[nodiscard]] Result<void> verify(Module &module);

} // namespace toolkit::sir
