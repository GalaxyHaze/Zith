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

[[nodiscard]] bool supportedLoadStoreWidth(Type type) noexcept {
    return type.kind == TypeKind::Bool || type.kind == TypeKind::Char ||
           type.kind == TypeKind::I1 || type.kind == TypeKind::I8 ||
           type.kind == TypeKind::I16 || type.kind == TypeKind::I32 ||
           type.kind == TypeKind::I64 || type.kind == TypeKind::F32 ||
           type.kind == TypeKind::F64;
}

[[nodiscard]] bool isBarrierOpcode(Opcode opcode) noexcept {
    return opcode == Opcode::Store || opcode == Opcode::Call ||
           opcode == Opcode::Branch || opcode == Opcode::CondBranch ||
           opcode == Opcode::Return;
}

[[nodiscard]] Value &appendValue(Function &function, ScopeData &scope, BlockData &block,
                                 Opcode opcode, Type type, Variable *variable = nullptr) {
    auto *value = function.arena->make<Value>();
    if (!value)
        constructError("allocating IR value");
    value->opcode = opcode;
    value->type = type;
    value->function = &function;
    value->owner = &scope;
    value->block = &block;
    value->variable = variable;
    function.values.push(value);
    if (block.values)
        block.values->push(value);
    return *value;
}

[[nodiscard]] BlockData &blockOfFunction(const Function &function, ScopeData &scope) {
    if (!function.currentBlock)
        constructError("function has no current block");
    if (function.currentBlock->function != &function)
        constructError("current block belongs to a different function");
    return *function.currentBlock;
}

[[nodiscard]] Value &valueOfVariable(const Function &function, ScopeData &scope, Variable &variable,
                                     BlockData &block) {
    if (!sameFunction(function, variable))
        constructError("variable belongs to a different function");
    if (variable.owner != &scope)
        constructError("variable is used outside the scope that declared it");
    for (auto *value : function.values) {
        if (value->variable == &variable && value->opcode == Opcode::Param)
            return *value;
    }
    auto &value = appendValue(const_cast<Function &>(function), scope, block, Opcode::Param,
                              variable.type, &variable);
    value.variable = &variable;
    return value;
}

[[nodiscard]] Value &materialize(Function &function, ScopeData &scope, BlockData &block,
                                 Operand operand, Type expected) {
    if (operand.hasValue) {
        if (operand.value == nullptr)
            constructError("null operand value");
        if (!sameFunction(function, *operand.value))
            constructError("value belongs to a different function");
        if (operand.value->block && operand.value->block->function != &function)
            constructError("value belongs to a block in a different function");
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
        return valueOfVariable(function, scope, *operand.variable, block);
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

[[nodiscard]] bool isBinaryOpcode(Opcode opcode) noexcept {
    switch (opcode) {
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Rem:
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::Shl:
    case Opcode::Shr:
    case Opcode::Eq:
    case Opcode::Ne:
    case Opcode::Lt:
    case Opcode::Le:
    case Opcode::Gt:
    case Opcode::Ge:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool isBitwiseOpcode(Opcode opcode) noexcept {
    return opcode == Opcode::BitAnd || opcode == Opcode::BitOr ||
           opcode == Opcode::BitXor || opcode == Opcode::Shl ||
           opcode == Opcode::Shr;
}

[[nodiscard]] Value &binary(Function &function, ScopeData &scope, BlockData &block, Opcode opcode,
                            Operand left, Operand right, Type resultType) {
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

    Value &leftValue = materialize(function, scope, block, left, inferred);
    Value &rightValue = materialize(function, scope, block, right, inferred);
    if (!leftValue.type.isNumeric() || !rightValue.type.isNumeric())
        constructError("arithmetic operands must be numeric");
    if (!(leftValue.type == rightValue.type))
        constructError("arithmetic operand types must agree");
    if (inferred == Type{})
        inferred = leftValue.type;
    if (!(inferred == leftValue.type))
        constructError("arithmetic result type must match operands");

    const bool bitwise = isBitwiseOpcode(opcode);
    if ((bitwise || opcode == Opcode::Shl || opcode == Opcode::Shr) &&
        !inferred.isInteger())
        constructError("bitwise and shift operations require integer types");

    const bool comparison = opcode == Opcode::Eq || opcode == Opcode::Ne ||
                            opcode == Opcode::Lt || opcode == Opcode::Le ||
                            opcode == Opcode::Gt || opcode == Opcode::Ge;
    if (comparison && !inferred.isNumeric())
        constructError("comparison operands must be numeric");

    auto &value = appendValue(function, scope, block, opcode,
                              comparison ? Primitives::i1 : inferred);
    value.left = &leftValue;
    value.right = &rightValue;
    return value;
}

[[nodiscard]] bool sameScopeOrChild(const ScopeData &scope, const ScopeData &candidate) noexcept {
    for (const ScopeData *current = &candidate; current; current = current->parent) {
        if (current == &scope)
            return true;
    }
    return false;
}

[[nodiscard]] Instruction &appendTerminator(Function &function, BlockData &block, Opcode opcode) {
    if (block.terminator)
        constructError("block already has a terminator");
    auto *instruction = function.arena->make<Instruction>();
    if (!instruction)
        constructError("allocating terminator instruction");
    instruction->opcode = opcode;
    instruction->function = &function;
    instruction->owner = block.parent;
    instruction->block = &block;
    block.terminator = instruction;
    function.instructions.push(instruction);
    return *instruction;
}

void appendReturn(Function &function, ScopeData &scope, BlockData &block, Type expected,
                  Operand operand) {
    if (!sameScopeOrChild(*function.baseScope, scope))
        constructError("return is outside the function scope");
    auto &instruction = appendTerminator(function, block, Opcode::Return);
    instruction.type = expected;

    if (expected.isVoid()) {
        if (operand.hasValue || operand.hasVariable)
            (void)materialize(function, scope, block, operand, Type{});
        instruction.value = nullptr;
        return;
    }

    if (!operand.hasValue && !operand.hasVariable && !operand.hasLiteral)
        constructError("non-void return needs a value");

    Value &value = materialize(function, scope, block, operand, expected);
    instruction.value = &value;
    instruction.variable = operand.hasVariable ? operand.variable : nullptr;
}

[[nodiscard]] CallArgs buildCallArgs(Function &caller, ScopeData &scope, BlockData &block,
                                     const Function &callee, std::span<const Operand> operands) {
    if (operands.size() != callee.argsDecl.count) {
        std::fprintf(stderr, "[error] sir: expected %zu arguments, got %zu\n",
                     callee.argsDecl.count, operands.size());
        constructError("argument count mismatch");
    }

    CallArgs result;
    result.interner = caller.interner;
    result.calleeName = callee.name;
    result.callee = const_cast<Function *>(&callee);
    result.signatureId = callee.argsDecl.signatureId;
    result.count = operands.size();
    for (size_t index = 0; index < operands.size(); ++index) {
        const Type expected = callee.argsDecl.typeAt(index);
        Value &value = materialize(caller, scope, block, operands[index], expected);
        result.values[index] = &value;
    }
    return result;
}

[[nodiscard]] bool hasTerminated(const Function &function) noexcept {
    for (auto *block : function.blocks)
        if (block && block->terminator && block->terminator->opcode == Opcode::Return)
            return true;
    return false;
}

void verifyValue(const Function &function, const Value &value);

void validateBlock(const Function &function, const BlockData &block) {
    if (block.function != &function)
        structuralError("function owns a block from another function");
    if (!block.terminator && block.id == function.nextBlockId - 1)
        structuralError("the last block has no terminator");
    if (!block.terminator)
        structuralError("block has no terminator");
    const auto opcode = block.terminator->opcode;
    if (opcode != Opcode::Return && opcode != Opcode::Branch && opcode != Opcode::CondBranch)
        structuralError("block terminator is not a control terminator");
    if (block.terminator->function != &function)
        structuralError("block terminator belongs to another function");
    if (opcode == Opcode::Return) {
        if (!(block.terminator->type == function.returnType))
            structuralError("terminator type does not match function return type");
        if (block.terminator->type.isVoid()) {
            if (block.terminator->value || block.terminator->variable)
                structuralError("void terminator carries an unexpected value");
            return;
        }
        if (block.terminator->value) {
            if (!sameFunction(function, *block.terminator->value))
                structuralError("terminator returns a foreign value");
            if (!(block.terminator->type == block.terminator->value->type))
                structuralError("terminator value type does not match function type");
        } else if (block.terminator->variable) {
            if (!sameFunction(function, *block.terminator->variable))
                structuralError("terminator returns a foreign variable");
            if (!(block.terminator->type == block.terminator->variable->type))
                structuralError("terminator variable type does not match function type");
        } else {
            structuralError("non-void terminator has no value");
        }
        return;
    }

    if (opcode == Opcode::Branch) {
        if (!block.trueTarget)
            structuralError("branch has a missing target");
        if (block.trueTarget->function != &function)
            structuralError("branch targets a foreign block");
        if (block.condition || block.falseTarget || block.terminator->condition ||
            block.terminator->trueTarget != block.trueTarget ||
            block.terminator->falseTarget)
            structuralError("branch terminator has inconsistent targets");
        return;
    }

    if (!block.trueTarget || !block.falseTarget)
        structuralError("conditional branch has a missing target");
    if (block.trueTarget->function != &function || block.falseTarget->function != &function)
        structuralError("conditional branch targets a foreign block");
    if (!block.condition || block.condition->function != &function)
        structuralError("conditional branch has a foreign condition");
    if (block.condition->type != Primitives::i1)
        structuralError("conditional branch condition must have i1 type");
    if (block.terminator->condition != block.condition)
        structuralError("conditional branch instruction has inconsistent condition");
    if (block.terminator->trueTarget != block.trueTarget ||
        block.terminator->falseTarget != block.falseTarget)
        structuralError("conditional branch instruction has inconsistent targets");
}

void verifyValue(const Function &function, const Value &value) {
    if (value.function != &function)
        structuralError("verification found a value owned by another function");
    if (value.owner && value.owner->function != &function)
        structuralError("value is owned by a scope in another function");
    if (value.block && value.block->function != &function)
        structuralError("value is owned by a block in another function");

    switch (value.opcode) {
    case Opcode::Constant:
        if (!value.type.isInteger() && !value.type.isFloat())
            structuralError("constant has a non-numeric type");
        break;
    case Opcode::Param:
        if (!value.variable || !sameFunction(function, *value.variable))
            structuralError("parameter value has a missing or foreign variable");
        break;
    case Opcode::Load:
        if (!value.address || value.address->function != &function)
            structuralError("load value has a missing or foreign address");
        if (value.address->opcode != Opcode::Param || !value.address->variable)
            structuralError("load address must be a variable value");
        if (value.type != value.address->variable->type)
            structuralError("load type does not match its address variable type");
        if (!supportedLoadStoreWidth(value.type))
            structuralError("load uses an unsupported width");
        break;
    case Opcode::Call: {
        if (!value.arguments || !value.arguments->callee)
            structuralError("call value has no argument list or callee");
        if (value.arguments->callee->owner != function.owner)
            structuralError("call targets a function in another module");
        if (value.arguments->count > CallArgs::maxArgs)
            structuralError("call has too many arguments");
        for (size_t index = 0; index < value.arguments->count; ++index) {
            Value *argument = value.arguments->values[index];
            if (!argument || !sameFunction(function, *argument))
                structuralError("call has a missing or foreign argument");
            const Type expected = value.arguments->callee->argsDecl.typeAt(index);
            if (argument->type != expected)
                structuralError("call argument type does not match callee signature");
        }
        break;
    }
    case Opcode::Store:
        if (!value.address || !value.address->variable ||
            value.address->function != &function)
            structuralError("store value has a missing or foreign address");
        if (value.address->opcode != Opcode::Param)
            structuralError("store address must be a variable value");
        if (!value.variable || value.variable != value.address->variable)
            structuralError("store value has an inconsistent variable");
        if (value.type != value.variable->type)
            structuralError("store type does not match its variable type");
        if (value.variable->isImmutable())
            structuralError("store targets an immutable variable");
        if (!supportedLoadStoreWidth(value.type))
            structuralError("store uses an unsupported width");
        break;
    case Opcode::Branch:
    case Opcode::CondBranch:
    case Opcode::Return:
        structuralError("control flow opcode appears in the value stream");
        break;
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Rem:
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::Shl:
    case Opcode::Shr:
    case Opcode::Eq:
    case Opcode::Ne:
    case Opcode::Lt:
    case Opcode::Le:
    case Opcode::Gt:
    case Opcode::Ge:
        if (value.left == nullptr || value.right == nullptr)
            structuralError("binary value has a missing operand");
        if (value.left->function != &function || value.right->function != &function)
            structuralError("binary value has a foreign operand");
        if (!value.left->type.isNumeric() || !value.right->type.isNumeric() ||
            !(value.left->type == value.right->type))
            structuralError("binary value has invalid operand types");
        if (isBitwiseOpcode(value.opcode) && !value.type.isInteger())
            structuralError("bitwise value has a non-integer result type");
        if ((value.opcode == Opcode::Eq || value.opcode == Opcode::Ne ||
             value.opcode == Opcode::Lt || value.opcode == Opcode::Le ||
             value.opcode == Opcode::Gt || value.opcode == Opcode::Ge) &&
            value.type != Primitives::i1)
            structuralError("comparison result must have i1 type");
        break;
    }
}

void verifyFunction(const Function &function) {
    if (!function.baseScope || !function.baseBlock)
        structuralError("function has no scope or block");
    if (!hasTerminated(function))
        structuralError("function has no return terminator");
    for (auto *value : function.values)
        verifyValue(function, *value);
    for (auto *variable : function.variables) {
        if (variable->function != &function)
            structuralError("function owns a variable from another function");
        if (variable->initializer && variable->initializer->type != variable->type)
            structuralError("variable initializer type does not match variable type");
        if (variable->owner && variable->owner->function != &function)
            structuralError("variable owner scope belongs to another function");
    }
    for (auto *block : function.blocks)
        validateBlock(function, *block);
}

} // namespace

void SirBuilder::failAllocation(std::string_view what) {
    constructError(what);
}

void Scope::failNull() {
    constructError("null scope handle");
}

void Block::failNull() {
    constructError("null block handle");
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
      params(arena), paramValues(arena), scopes(arena), blocks(arena), variables(arena),
      values(arena), instructions(arena) {}

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
    scope->block = currentBlock;
    scopes.push(scope);
    currentScope = scope;
    if (!baseScope)
        baseScope = scope;
    return Scope{scope};
}

Block Function::pushBlock() {
    auto *block = arena->make<BlockData>();
    if (!block)
        constructError("allocating block");
    block->arena = arena;
    block->function = this;
    block->parent = currentScope;
    block->id = nextBlockId++;
    block->values = arena->make<DynArray<Value *>>(*arena);
    block->scopes = arena->make<DynArray<ScopeData *>>(*arena);
    if (!block->values || !block->scopes)
        constructError("allocating block storage");
    blocks.push(block);
    if (!baseBlock)
        baseBlock = block;
    currentBlock = block;
    if (currentScope)
        currentScope->block = block;
    return Block{block};
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
    return binary(*this, current(), blockOfFunction(*this, current()), Opcode::Add, left, right,
                  resultType);
}

Value &Function::sub(Operand left, Operand right, Type resultType) {
    return binary(*this, current(), blockOfFunction(*this, current()), Opcode::Sub, left, right,
                  resultType);
}

Value &Function::mul(Operand left, Operand right, Type resultType) {
    return binary(*this, current(), blockOfFunction(*this, current()), Opcode::Mul, left, right,
                  resultType);
}

Value &Function::div(Operand left, Operand right, Type resultType) {
    return binary(*this, current(), blockOfFunction(*this, current()), Opcode::Div, left, right,
                  resultType);
}

Value &Function::rem(Operand left, Operand right, Type resultType) {
    return binary(*this, current(), blockOfFunction(*this, current()), Opcode::Rem, left, right,
                  resultType);
}

Value &Function::bitAnd(Operand left, Operand right, Type resultType) {
    return binary(*this, current(), blockOfFunction(*this, current()), Opcode::BitAnd, left, right,
                  resultType);
}

Value &Function::bitOr(Operand left, Operand right, Type resultType) {
    return binary(*this, current(), blockOfFunction(*this, current()), Opcode::BitOr, left, right,
                  resultType);
}

Value &Function::bitXor(Operand left, Operand right, Type resultType) {
    return binary(*this, current(), blockOfFunction(*this, current()), Opcode::BitXor, left, right,
                  resultType);
}

Value &Function::shl(Operand left, Operand right, Type resultType) {
    return binary(*this, current(), blockOfFunction(*this, current()), Opcode::Shl, left, right,
                  resultType);
}

Value &Function::shr(Operand left, Operand right, Type resultType) {
    return binary(*this, current(), blockOfFunction(*this, current()), Opcode::Shr, left, right,
                  resultType);
}

Value &Function::eq(Operand left, Operand right) {
    return binary(*this, current(), blockOfFunction(*this, current()), Opcode::Eq, left, right, {});
}

Value &Function::ne(Operand left, Operand right) {
    return binary(*this, current(), blockOfFunction(*this, current()), Opcode::Ne, left, right, {});
}

Value &Function::lt(Operand left, Operand right) {
    return binary(*this, current(), blockOfFunction(*this, current()), Opcode::Lt, left, right, {});
}

Value &Function::le(Operand left, Operand right) {
    return binary(*this, current(), blockOfFunction(*this, current()), Opcode::Le, left, right, {});
}

Value &Function::gt(Operand left, Operand right) {
    return binary(*this, current(), blockOfFunction(*this, current()), Opcode::Gt, left, right, {});
}

Value &Function::ge(Operand left, Operand right) {
    return binary(*this, current(), blockOfFunction(*this, current()), Opcode::Ge, left, right, {});
}

Value &Function::load(Variable &variable, Type resultType) {
    return current().load(variable, resultType);
}

void Function::store(Variable &variable, Operand value) {
    current().store(variable, value);
}

Value &Function::call(Function &callee, std::span<const Operand> arguments, Type resultType) {
    return current().call(callee, arguments, resultType);
}

Value &Function::call(InternedId calleeName, std::span<const Operand> arguments, Type resultType) {
    return current().call(calleeName, arguments, resultType);
}

void Function::ret(Operand value) {
    appendReturn(*this, current(), blockOfFunction(*this, current()), returnType, value);
}

void Function::retVoid() {
    appendReturn(*this, current(), blockOfFunction(*this, current()), returnType, {});
}

void Function::selectBlock(Block &block) {
    current().selectBlock(block);
}

void Function::br(Block &target) {
    current().br(target);
}

void Function::condBranch(Operand condition, Block &trueTarget, Block &falseTarget) {
    current().condBranch(condition, trueTarget, falseTarget);
}

CallArgs Function::makeArgs(Operand first) {
    const std::array operands{first};
    return buildCallArgs(*this, current(), blockOfFunction(*this, current()), *this, operands);
}

CallArgs Function::makeArgs(Operand first, Operand second) {
    const std::array operands{first, second};
    return buildCallArgs(*this, current(), blockOfFunction(*this, current()), *this, operands);
}

CallArgs Function::makeArgs(Operand first, Operand second, Operand third) {
    const std::array operands{first, second, third};
    return buildCallArgs(*this, current(), blockOfFunction(*this, current()), *this, operands);
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
    variable->block = block;
    function->variables.push(variable);
    return *variable;
}

Value &ScopeData::constInt64(int64_t value, Type type) {
    if (!type.isInteger())
        constructError("integer constant requires an integer type");
    auto &result = appendValue(*function, *this, blockOfFunction(*function, *this),
                               Opcode::Constant, type);
    result.intValue = value;
    return result;
}

Value &ScopeData::constFloat64(double value, Type type) {
    if (!floatType(type))
        constructError("float constant requires a float type");
    auto &result = appendValue(*function, *this, blockOfFunction(*function, *this),
                               Opcode::Constant, type);
    result.floatValue = value;
    return result;
}

Value &ScopeData::add(Operand left, Operand right, Type resultType) {
    return binary(*function, *this, blockOfFunction(*function, *this), Opcode::Add, left, right,
                  resultType);
}

Value &ScopeData::sub(Operand left, Operand right, Type resultType) {
    return binary(*function, *this, blockOfFunction(*function, *this), Opcode::Sub, left, right,
                  resultType);
}

Value &ScopeData::mul(Operand left, Operand right, Type resultType) {
    return binary(*function, *this, blockOfFunction(*function, *this), Opcode::Mul, left, right,
                  resultType);
}

Value &ScopeData::div(Operand left, Operand right, Type resultType) {
    return binary(*function, *this, blockOfFunction(*function, *this), Opcode::Div, left, right,
                  resultType);
}

Value &ScopeData::rem(Operand left, Operand right, Type resultType) {
    return binary(*function, *this, blockOfFunction(*function, *this), Opcode::Rem, left, right,
                  resultType);
}

Value &ScopeData::bitAnd(Operand left, Operand right, Type resultType) {
    return binary(*function, *this, blockOfFunction(*function, *this), Opcode::BitAnd, left, right,
                  resultType);
}

Value &ScopeData::bitOr(Operand left, Operand right, Type resultType) {
    return binary(*function, *this, blockOfFunction(*function, *this), Opcode::BitOr, left, right,
                  resultType);
}

Value &ScopeData::bitXor(Operand left, Operand right, Type resultType) {
    return binary(*function, *this, blockOfFunction(*function, *this), Opcode::BitXor, left, right,
                  resultType);
}

Value &ScopeData::shl(Operand left, Operand right, Type resultType) {
    return binary(*function, *this, blockOfFunction(*function, *this), Opcode::Shl, left, right,
                  resultType);
}

Value &ScopeData::shr(Operand left, Operand right, Type resultType) {
    return binary(*function, *this, blockOfFunction(*function, *this), Opcode::Shr, left, right,
                  resultType);
}

Value &ScopeData::eq(Operand left, Operand right) {
    return binary(*function, *this, blockOfFunction(*function, *this), Opcode::Eq, left, right, {});
}

Value &ScopeData::ne(Operand left, Operand right) {
    return binary(*function, *this, blockOfFunction(*function, *this), Opcode::Ne, left, right, {});
}

Value &ScopeData::lt(Operand left, Operand right) {
    return binary(*function, *this, blockOfFunction(*function, *this), Opcode::Lt, left, right, {});
}

Value &ScopeData::le(Operand left, Operand right) {
    return binary(*function, *this, blockOfFunction(*function, *this), Opcode::Le, left, right, {});
}

Value &ScopeData::gt(Operand left, Operand right) {
    return binary(*function, *this, blockOfFunction(*function, *this), Opcode::Gt, left, right, {});
}

Value &ScopeData::ge(Operand left, Operand right) {
    return binary(*function, *this, blockOfFunction(*function, *this), Opcode::Ge, left, right, {});
}

Value &ScopeData::load(Variable &variable, Type resultType) {
    if (!sameFunction(*function, variable))
        constructError("load targets a variable from another function");
    if (variable.owner != this)
        constructError("load targets a variable outside the current scope");
    if (!supportedLoadStoreWidth(variable.type))
        constructError("load uses an unsupported width");
    Value &address = valueOfVariable(*function, *this, variable,
                                     blockOfFunction(*function, *this));
    if (resultType == Type{})
        resultType = variable.type;
    if (resultType != variable.type)
        constructError("load result type must match the variable type");
    Value &loaded = appendValue(*function, *this, blockOfFunction(*function, *this),
                                Opcode::Load, resultType, &variable);
    loaded.address = &address;
    return loaded;
}

void ScopeData::store(Variable &variable, Operand value) {
    if (!sameFunction(*function, variable))
        constructError("store targets a variable from another function");
    if (variable.owner != this)
        constructError("store targets a variable outside the current scope");
    if (variable.isImmutable())
        constructError("cannot write to an immutable variable");
    if (!supportedLoadStoreWidth(variable.type))
        constructError("store uses an unsupported width");
    Value &storedValue = materialize(*function, *this, blockOfFunction(*function, *this), value,
                                     variable.type);
    variable.initializer = &storedValue;
    auto *store = function->arena->make<Value>();
    if (!store)
        constructError("allocating store value");
    store->opcode = Opcode::Store;
    store->type = variable.type;
    store->function = function;
    store->owner = this;
    store->block = &blockOfFunction(*function, *this);
    store->variable = &variable;
    store->address = &valueOfVariable(*function, *this, variable,
                                      blockOfFunction(*function, *this));
    function->values.push(store);
    if (block && block->values)
        block->values->push(store);
}

Value &ScopeData::call(Function &callee, std::span<const Operand> arguments, Type resultType) {
    BlockData &block = blockOfFunction(*function, *this);
    CallArgs args = buildCallArgs(*function, *this, block, callee, arguments);
    if (resultType == Type{})
        resultType = callee.returnType;
    Value &callValue = appendValue(*function, *this, block, Opcode::Call, resultType);
    callValue.callee = &callee;
    callValue.arguments = function->arena->make<CallArgs>(std::move(args));
    if (!callValue.arguments)
        constructError("allocating call arguments");
    return callValue;
}

Value &ScopeData::call(InternedId calleeName, std::span<const Operand> arguments,
                       Type resultType) {
    if (!function->owner)
        constructError("call by name requires an owning module");
    Function *callee = nullptr;
    const std::string_view calleeView = function->interner ? function->interner->lookup(calleeName)
                                                          : std::string_view{};
    for (auto *candidate : function->owner->functions) {
        if (candidate && candidate->name == calleeName)
            callee = candidate;
    }
    if (!callee)
        constructError("call by name does not resolve to a declared function");
    BlockData &block = blockOfFunction(*function, *this);
    CallArgs args = buildCallArgs(*function, *this, block, *callee, arguments);
    if (resultType == Type{})
        resultType = callee->returnType;
    Value &callValue = appendValue(*function, *this, block, Opcode::Call, resultType, nullptr);
    callValue.callee = callee;
    callValue.arguments = function->arena->make<CallArgs>(std::move(args));
    if (!callValue.arguments)
        constructError("allocating call arguments");
    callValue.arguments->calleeName = calleeName;
    callValue.arguments->callee = callee;
    callValue.arguments->interner = function->interner;
    (void)calleeView;
    return callValue;
}

void ScopeData::ret(Operand value) {
    appendReturn(*function, *this, blockOfFunction(*function, *this), function->returnType, value);
}

void ScopeData::retVoid() {
    appendReturn(*function, *this, blockOfFunction(*function, *this), function->returnType, {});
}

Block ScopeData::pushBlock() {
    return function->pushBlock();
}

void ScopeData::selectBlock(Block &block) {
    if (!block || block->function != function)
        constructError("cannot select a null or foreign block");
    function->currentBlock = block.getPtr();
    this->block = block.getPtr();
    block->parent = this;
}

void ScopeData::br(Block &target) {
    if (!target || target->function != function)
        constructError("branch targets a null or foreign block");
    BlockData &source = blockOfFunction(*function, *this);
    auto &instruction = appendTerminator(*function, source, Opcode::Branch);
    instruction.trueTarget = target.getPtr();
    source.trueTarget = target.getPtr();
}

void ScopeData::condBranch(Operand conditionOperand, Block &trueTarget, Block &falseTarget) {
    if (!trueTarget || !falseTarget || trueTarget->function != function ||
        falseTarget->function != function)
        constructError("conditional branch targets a null or foreign block");
    BlockData &source = blockOfFunction(*function, *this);
    Value &condition = materialize(*function, *this, source, conditionOperand, Primitives::i1);
    auto &instruction = appendTerminator(*function, source, Opcode::CondBranch);
    instruction.condition = &condition;
    instruction.trueTarget = trueTarget.getPtr();
    instruction.falseTarget = falseTarget.getPtr();
    source.condition = &condition;
    source.trueTarget = trueTarget.getPtr();
    source.falseTarget = falseTarget.getPtr();
}

Function &Module::declareFn(std::string_view name) {
    return declareFn(name, Primitives::voidT, ArgsDecl{});
}

Function &Module::declareFn(std::string_view name, Type returnType, ArgsDecl args) {
    auto *function = arena->make<Function>(*arena, *interner, name, returnType);
    if (!function)
        constructError("allocating function");
    function->owner = this;
    function->argsDecl = args;
    Block root = function->pushBlock();
    Scope rootScope = function->pushScope();
    ScopeData &rootData = *rootScope;
    rootData.block = root.getPtr();

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
        variable->block = root.getPtr();
        function->params.push(variable);
        function->variables.push(variable);

        Value &paramValue =
            appendValue(*function, rootData, *root.getPtr(), Opcode::Param, variable->type,
                        variable);
        paramValue.variable = variable;
        function->paramValues.push(&paramValue);
    }

    function->currentScope = &rootData;
    function->currentBlock = root.getPtr();
    functions.push(function);
    return *function;
}

Type &Module::arrayType(const Type &element, std::uint64_t length) {
    auto *type = arena->make<Type>(TypeKind::Array, const_cast<Type *>(&element), length);
    if (!type)
        constructError("allocating array type");
    types.push(type);
    return *type;
}

Type &Module::sliceType(const Type &element) {
    auto *type = arena->make<Type>(TypeKind::Slice, const_cast<Type *>(&element));
    if (!type)
        constructError("allocating slice type");
    types.push(type);
    return *type;
}

Type &Module::pointerType(const Type &pointee) {
    auto *type = arena->make<Type>(TypeKind::Pointer, const_cast<Type *>(&pointee));
    if (!type)
        constructError("allocating pointer type");
    types.push(type);
    return *type;
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
