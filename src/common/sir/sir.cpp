#include "common/sir/sir.hpp"

#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"
#include "common/memory/result.hpp"
#include "common/memory/string-interner.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace toolkit::sir {

namespace {

[[noreturn]] void constructError(std::string_view message) {
    std::fprintf(stderr, "[error] sir: %.*s\n", static_cast<int>(message.size()), message.data());
    std::abort();
}

[[noreturn]] void structuralError(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

[[nodiscard]] bool sameFunction(const Function &function, const Value &value) noexcept {
    return value.function == &function;
}

[[nodiscard]] bool sameFunction(const Function &function, const Variable &variable) noexcept {
    return variable.function == &function;
}

[[nodiscard]] bool floatType(Type type) noexcept {
    return type.kind == TypeKind::F32 || type.kind == TypeKind::F64;
}

[[nodiscard]] Value &appendValue(Function &function, ScopeData &scope, Opcode opcode, Type type,
                                 Variable *variable = nullptr) {
    auto *value = function.arena->make<Value>();
    if (!value)
        constructError("allocating IR value");
    value->opcode = opcode;
    value->type = type;
    value->function = &function;
    value->owner = &scope;
    value->variable = variable;
    function.values.push(value);
    return *value;
}

[[nodiscard]] Value &valueOfVariable(const Function &function, ScopeData &scope, Variable &variable) {
    if (!sameFunction(function, variable))
        constructError("variable belongs to a different function");
    if (variable.owner != &scope)
        constructError("variable is used outside the scope that declared it");
    for (auto *value : function.values) {
        if (value->variable == &variable)
            return *value;
    }
    auto &value = appendValue(const_cast<Function &>(function), scope, Opcode::Param,
                              variable.type, &variable);
    value.variable = &variable;
    return value;
}

[[nodiscard]] Value &materialize(Function &function, ScopeData &scope, Operand operand,
                                 Type expected) {
    if (operand.hasValue) {
        if (operand.value == nullptr)
            constructError("null operand value");
        if (!sameFunction(function, *operand.value))
            constructError("value belongs to a different function");
        if (!(expected == Type{}) && !(operand.value->type == expected))
            constructError("operand type does not match the expected type");
        return *operand.value;
    }

    if (operand.hasVariable) {
        if (operand.variable == nullptr)
            constructError("null operand variable");
        if (!sameFunction(function, *operand.variable))
            constructError("variable belongs to a different function");
        if (operand.variable->owner != &scope)
            constructError("variable is used outside the scope that declared it");
        if (!(expected == Type{}) && !(operand.variable->type == expected))
            constructError("operand type does not match the expected type");
        return valueOfVariable(function, scope, *operand.variable);
    }

    if (operand.hasLiteral) {
        if (expected == Type{})
            expected = operand.floatLiteralOnly ? Primitives::f64 : Primitives::i32;
        if (operand.floatLiteralOnly) {
            if (!floatType(expected))
                constructError("float literal requires a float parameter type");
            return scope.constFloat64(operand.floatLiteral, expected);
        }
        if (!expected.isInteger())
            constructError("integer literal requires an integer parameter type");
        return scope.constInt64(operand.intLiteral, expected);
    }

    constructError("operand has no value");
}

[[nodiscard]] Value &binary(Function &function, ScopeData &scope, Opcode opcode, Operand left,
                            Operand right, Type resultType) {
    Type inferred = resultType;
    if (inferred == Type{}) {
        const Operand *hint = left.hasValue ? &left : left.hasVariable ? &left
                              : right.hasValue  ? &right
                              : right.hasVariable ? &right
                                                  : nullptr;
        if (hint) {
            inferred = hint->hasValue    ? hint->value->type
                       : hint->hasVariable ? hint->variable->type
                                            : Type{};
        } else {
            inferred = left.floatLiteralOnly ? Primitives::f64 : Primitives::i32;
        }
    }

    Value &leftValue = materialize(function, scope, left, inferred);
    Value &rightValue = materialize(function, scope, right, inferred);
    if (!leftValue.type.isNumeric() || !rightValue.type.isNumeric())
        constructError("arithmetic operands must be numeric");
    if (!(leftValue.type == rightValue.type))
        constructError("arithmetic operand types must agree");
    if (inferred == Type{})
        inferred = leftValue.type;
    if (!(inferred == leftValue.type))
        constructError("arithmetic result type must match operands");

    auto &value = appendValue(function, scope, opcode, inferred);
    value.left = &leftValue;
    value.right = &rightValue;
    return value;
}

void appendReturn(Function &function, ScopeData &scope, Type expected, Operand operand) {
    auto *instruction = function.arena->make<Instruction>();
    if (!instruction)
        constructError("allocating return instruction");
    instruction->opcode = Opcode::Return;
    instruction->type = expected;
    instruction->function = &function;
    instruction->owner = &scope;

    if (expected.isVoid()) {
        if (operand.hasValue || operand.hasVariable)
            (void)materialize(function, scope, operand, Type{});
        instruction->value = nullptr;
        function.instructions.push(instruction);
        return;
    }

    if (!operand.hasValue && !operand.hasVariable && !operand.hasLiteral)
        constructError("non-void return needs a value");

    Value &value = materialize(function, scope, operand, expected);
    instruction->value = &value;
    instruction->variable = operand.hasVariable ? operand.variable : nullptr;
    function.instructions.push(instruction);
}

[[nodiscard]] CallArgs buildCallArgs(Function &function, std::span<const Operand> operands) {
    if (operands.size() != function.argsDecl.count) {
        std::fprintf(stderr, "[error] sir: expected %zu arguments, got %zu\n",
                     function.argsDecl.count, operands.size());
        constructError("argument count mismatch");
    }

    CallArgs result;
    result.signatureId = function.argsDecl.signatureId;
    result.count = operands.size();
    for (size_t index = 0; index < operands.size(); ++index) {
        const Type expected = function.argsDecl.typeAt(index);
        Value &value = materialize(function, function.current(), operands[index], expected);
        result.values[index] = &value;
    }
    return result;
}

[[nodiscard]] bool hasReturn(const Function &function) noexcept {
    for (auto *instruction : function.instructions)
        if (instruction->opcode == Opcode::Return)
            return true;
    return false;
}

void verifyValue(const Function &function, const Value &value) {
    if (value.function != &function)
        structuralError("verification found a value owned by another function");
    switch (value.opcode) {
    case Opcode::Constant:
        if (!value.type.isInteger() && !value.type.isFloat())
            structuralError("constant has a non-numeric type");
        break;
    case Opcode::Store:
    case Opcode::Param:
        break;
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
        if (!value.left || !value.right)
            structuralError("arithmetic value has a missing operand");
        if (value.left->function != &function || value.right->function != &function)
            structuralError("arithmetic value has a foreign operand");
        if (!value.left->type.isNumeric() || !value.right->type.isNumeric() ||
            !(value.left->type == value.right->type) || !(value.type == value.left->type))
            structuralError("arithmetic value has invalid operand types");
        break;
    case Opcode::Return:
        break;
    }
}

void verifyFunction(const Function &function) {
    if (!function.baseScope)
        structuralError("function has no scope");
    if (!hasReturn(function))
        structuralError("function has no terminator");
    for (auto *value : function.values)
        verifyValue(function, *value);
    for (auto *variable : function.variables) {
        if (variable->function != &function)
            structuralError("function owns a variable from another function");
        if (variable->initializer && variable->initializer->type != variable->type)
            structuralError("variable initializer type does not match variable type");
    }
    for (auto *instruction : function.instructions) {
        if (instruction->opcode != Opcode::Return)
            structuralError("instruction stream contains a non-terminator");
        if (instruction->function != &function)
            structuralError("instruction stream contains a foreign instruction");
        if (!(instruction->type == function.returnType))
            structuralError("terminator type does not match function return type");
        if (instruction->type.isVoid()) {
            if (instruction->value || instruction->variable)
                structuralError("void terminator carries an unexpected value");
            continue;
        }
        if (instruction->value) {
            if (!sameFunction(function, *instruction->value))
                structuralError("terminator returns a foreign value");
            if (!(instruction->type == instruction->value->type))
                structuralError("terminator value type does not match function type");
        } else if (instruction->variable) {
            if (!sameFunction(function, *instruction->variable))
                structuralError("terminator returns a foreign variable");
            if (!(instruction->type == instruction->variable->type))
                structuralError("terminator variable type does not match function type");
        } else {
            structuralError("non-void terminator has no value");
        }
    }
}

} // namespace

void SirBuilder::failAllocation(std::string_view what) {
    constructError(what);
}

void Scope::failNull() {
    constructError("null scope handle");
}

template <Type Ty>
Variable &Variable::makeConstant(int64_t value) {
    if (!function)
        constructError("variable has no owning function");
    initializer = &function->constInt64(value, Ty);
    return *this;
}

Variable &Variable::makeConstant(Type type, int64_t value) {
    if (!function)
        constructError("variable has no owning function");
    initializer = &function->constInt64(value, type);
    return *this;
}

Variable &Variable::makeConstantFloat(Type type, double value) {
    if (!function)
        constructError("variable has no owning function");
    initializer = &function->constFloat64(value, type);
    return *this;
}

Variable &Variable::storeConstant(int64_t value) {
    if (!function)
        constructError("variable has no owning function");
    if (isImmutable())
        constructError("cannot write to an immutable variable");
    if (!type.isInteger())
        constructError("integer store requires an integer variable");
    initializer = &function->constInt64(value, type);
    return *this;
}

std::string_view Variable::nameView() const noexcept {
    return interner ? interner->lookup(name) : std::string_view{};
}

Function::Function(Arena &arena, StringInterner &interner_, std::string_view name_,
                   Type returnType_)
    : arena(&arena), interner(&interner_), name(interner_.intern(name_)), returnType(returnType_),
      params(arena),
      paramValues(arena), scopes(arena), variables(arena), values(arena), instructions(arena) {}

std::string_view Function::nameView() const noexcept {
    return interner ? interner->lookup(name) : std::string_view{};
}

Scope Function::pushScope() {
    auto *scope = arena->make<ScopeData>();
    if (!scope)
        constructError("allocating scope");
    scope->arena = arena;
    scope->function = this;
    scope->parent = currentScope;
    scopes.push(scope);
    currentScope = scope;
    if (!baseScope)
        baseScope = scope;
    return Scope{scope};
}

Value &Function::param(size_t index) {
    if (index >= paramValues.size())
        constructError("parameter index out of range");
    return *paramValues[index];
}

Value &Function::constInt64(int64_t value, Type type) {
    return current().constInt64(value, type);
}

Value &Function::constFloat64(double value, Type type) {
    return current().constFloat64(value, type);
}

Value &Function::add(Operand left, Operand right, Type resultType) {
    return binary(*this, current(), Opcode::Add, left, right, resultType);
}

Value &Function::sub(Operand left, Operand right, Type resultType) {
    return binary(*this, current(), Opcode::Sub, left, right, resultType);
}

Value &Function::mul(Operand left, Operand right, Type resultType) {
    return binary(*this, current(), Opcode::Mul, left, right, resultType);
}

void Function::store(Variable &variable, Operand value) {
    current().store(variable, value);
}

void Function::ret(Operand value) {
    appendReturn(*this, current(), returnType, value);
}

void Function::retVoid() {
    appendReturn(*this, current(), returnType, {});
}

CallArgs Function::makeArgs(Operand first) {
    const std::array operands{first};
    return buildCallArgs(*this, operands);
}

CallArgs Function::makeArgs(Operand first, Operand second) {
    const std::array operands{first, second};
    return buildCallArgs(*this, operands);
}

CallArgs Function::makeArgs(Operand first, Operand second, Operand third) {
    const std::array operands{first, second, third};
    return buildCallArgs(*this, operands);
}

ScopeData &Function::current() {
    if (!currentScope)
        constructError("function has no current scope");
    return *currentScope;
}

Variable &ScopeData::declVar(std::string_view name, Type type) {
    auto *variable = arena->make<Variable>();
    if (!variable)
        constructError("allocating variable");
    variable->interner = function->interner;
    variable->arena = arena;
    variable->name = function->interner->intern(name);
    variable->type = type;
    variable->function = function;
    variable->owner = this;
    function->variables.push(variable);
    return *variable;
}

Value &ScopeData::constInt64(int64_t value, Type type) {
    if (!type.isInteger())
        constructError("integer constant requires an integer type");
    auto &result = appendValue(*function, *this, Opcode::Constant, type);
    result.intValue = value;
    return result;
}

Value &ScopeData::constFloat64(double value, Type type) {
    if (!floatType(type))
        constructError("float constant requires a float type");
    auto &result = appendValue(*function, *this, Opcode::Constant, type);
    result.floatValue = value;
    return result;
}

Value &ScopeData::add(Operand left, Operand right, Type resultType) {
    return binary(*function, *this, Opcode::Add, left, right, resultType);
}

Value &ScopeData::sub(Operand left, Operand right, Type resultType) {
    return binary(*function, *this, Opcode::Sub, left, right, resultType);
}

Value &ScopeData::mul(Operand left, Operand right, Type resultType) {
    return binary(*function, *this, Opcode::Mul, left, right, resultType);
}

void ScopeData::store(Variable &variable, Operand value) {
    if (!sameFunction(*function, variable))
        constructError("store targets a variable from another function");
    if (variable.owner != this)
        constructError("store targets a variable outside the current scope");
    if (variable.isImmutable())
        constructError("cannot write to an immutable variable");
    variable.initializer = &materialize(*function, *this, value, variable.type);
}

void ScopeData::ret(Operand value) {
    appendReturn(*function, *this, function->returnType, value);
}

void ScopeData::retVoid() {
    appendReturn(*function, *this, function->returnType, {});
}

Function &Module::declareFn(std::string_view name) {
    return declareFn(name, Primitives::voidT, ArgsDecl{});
}

Function &Module::declareFn(std::string_view name, Type returnType, ArgsDecl args) {
    auto *function = arena->make<Function>(*arena, *interner, name, returnType);
    if (!function)
        constructError("allocating function");
    function->argsDecl = args;
    Scope root = function->pushScope();
    ScopeData &rootData = *root;

    for (size_t index = 0; index < args.count; ++index) {
        auto *variable = function->arena->make<Variable>();
        if (!variable)
            constructError("allocating parameter");
        char localName[32];
        std::snprintf(localName, sizeof(localName), "arg%zu", index);
        variable->interner = function->interner;
        variable->arena = function->arena;
        variable->name = function->interner->intern(localName);
        variable->type = args.typeAt(index);
        variable->function = function;
        variable->owner = &rootData;
        function->params.push(variable);
        function->variables.push(variable);

        Value &paramValue =
            appendValue(*function, rootData, Opcode::Param, variable->type, variable);
        paramValue.variable = variable;
        function->paramValues.push(&paramValue);
    }

    function->currentScope = &rootData;
    functions.push(function);
    return *function;
}

Result<void> verify(Module &module) {
    if (!module.interner)
        return common::memory::Error{"sir module has no interner"};
    try {
        for (auto *function : module.functions) {
            if (!function)
                return common::memory::Error{"sir module contains a null function"};
            verifyFunction(*function);
        }
    } catch (const std::runtime_error &error) {
        return common::memory::Error{error.what()};
    }
    return {};
}

template Variable &Variable::makeConstant<Primitives::i1>(int64_t);
template Variable &Variable::makeConstant<Primitives::i8>(int64_t);
template Variable &Variable::makeConstant<Primitives::i16>(int64_t);
template Variable &Variable::makeConstant<Primitives::i32>(int64_t);
template Variable &Variable::makeConstant<Primitives::i64>(int64_t);

} // namespace toolkit::sir
