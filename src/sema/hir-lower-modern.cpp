#include "sema/hir-lower-modern.hpp"

#include <filesystem>

#include "common/overloaded.hpp"
#include "diagnostics/error-codes.hpp"
#include "hir/hir-attrs.hpp"
#include "sema/nra-facts.hpp"
#include "sema/op-mapping.hpp"
#include "support/debug-print.hpp"
#include "support/int-literal.hpp"

#include "types/type-kind.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <span>
#include <string>

namespace zith::sema::modern {

namespace {

/// Decodes the C-like escape set inside string/char literal bodies. Returns false
/// (without touching `output`) when an escape is malformed or unknown.
bool decodeEscapes(std::string_view text, std::string &output) {
    for (size_t index = 0; index < text.size(); ++index) {
        const char current = text[index];
        if (current != '\\') {
            output.push_back(current);
            continue;
        }
        if (index + 1 >= text.size())
            return false;
        const char escaped = text[++index];
        switch (escaped) {
        case 'n':
            output.push_back('\n');
            break;
        case 'r':
            output.push_back('\r');
            break;
        case 't':
            output.push_back('\t');
            break;
        case '0':
            output.push_back('\0');
            break;
        case '\\':
            output.push_back('\\');
            break;
        case '#':
            output.push_back('#');
            break;
        case '\'':
            output.push_back('\'');
            break;
        case '"':
            output.push_back('"');
            break;
        case 'x': {
            if (index + 2 >= text.size() ||
                !std::isxdigit(static_cast<unsigned char>(text[index + 1])) ||
                !std::isxdigit(static_cast<unsigned char>(text[index + 2]))) {
                return false;
            }
            const unsigned high = static_cast<unsigned char>(text[index + 1]);
            const unsigned low  = static_cast<unsigned char>(text[index + 2]);
            const auto hex      = [](unsigned c) {
                return c >= '0' && c <= '9'   ? c - '0'
                            : c >= 'a' && c <= 'f' ? c - 'a' + 10U
                                                   : c - 'A' + 10U;
            };
            output.push_back(static_cast<char>((hex(high) << 4U) | hex(low)));
            index += 2U;
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

uint64_t internFunctionKey(memory::StringInterner &interner, std::string_view module,
                           frontend::DeclId decl) {
    const uint64_t module_id = interner.intern(module);
    return (module_id << 32U) | decl.value;
}

/// Dot-separated module namespace for `module_key`, derived from the longest
/// configured root that prefixes it: `stdlib/std/io/console.zith` under the
/// stdlib root `stdlib` becomes `std.io.console`.  Falls back to the file stem.
std::string moduleNamespace(std::string_view module_key, const session::CacheKey &cache_key) {
    const std::filesystem::path path{module_key};
    std::string best_relative;
    size_t best_root_length = 0;
    const auto consider     = [&](const std::string &root) {
        if (root.empty())
            return;
        std::error_code ec;
        const auto relative = std::filesystem::relative(path, std::filesystem::path(root), ec);
        if (ec || relative.empty())
            return;
        const auto text = relative.generic_string();
        // A relative path escaping the root is not "inside" it.
        if (text.starts_with(".."))
            return;
        if (root.size() >= best_root_length) {
            best_root_length = root.size();
            best_relative    = text;
        }
    };
    for (const auto &root : cache_key.stdlibRoots)
        consider(root);
    for (const auto &root : cache_key.includeRoots)
        consider(root);
    consider(cache_key.workspaceRoot);

    std::string relative = best_relative.empty() ? path.stem().string() : best_relative;
    if (relative.size() > 5U && relative.compare(relative.size() - 5U, 5U, ".zith") == 0)
        relative.erase(relative.size() - 5U);
    for (auto &character : relative) {
        if (character == '/' || character == '\\')
            character = '.';
    }
    return relative;
}

} // namespace

HirLowerModern::HirLowerModern(memory::Arena &arena, diagnostics::DiagnosticEngine &diags,
                               const session::CompilationSnapshot &snapshot, SemaPipeline &sema,
                               types::TypeIntern &types, memory::StringInterner &interner,
                               const NraFacts *nra)
    : arena_(arena), diags_(diags), snapshot_(snapshot), sema_(sema), types_(types),
      interner_(interner), nra_(nra), hir_(arena), lowered_types_(), function_index_by_key_() {}

bool HirLowerModern::run() {
    return predeclareGlobalConsts() && predeclareFunctions() && lowerFunctionBodies() &&
           !diags_.hasErrors();
}

bool HirLowerModern::predeclareGlobalConsts() {
    for (const auto &module_ptr : snapshot_.modules()) {
        const auto &module = *module_ptr;
        auto *module_sema  = sema_.findModuleSema(module.key);
        if (module_sema == nullptr)
            continue;

        for (const auto &edge : snapshot_.importGraph()) {
            if (edge.importer != module.key ||
                edge.targetKind != session::ImportTargetKind::CHeader || edge.cHeader == nullptr ||
                !edge.error.empty())
                continue;
            const auto name_space = moduleNamespace(module.key, snapshot_.cacheKey());
            for (const auto &constant : edge.cHeader->constants) {
                const auto key = interner_.intern(constant.name);
                if (global_const_by_name_.get(key) != nullptr)
                    continue;

                std::string global_name = "_zith_";
                if (!name_space.empty()) {
                    global_name += name_space;
                    global_name += '.';
                }
                global_name += constant.name;

                auto &global = hir_.addGlobalConst();
                global.name  = interner_.intern(global_name);
                switch (constant.kind) {
                case cinterop::ConstantKind::Integer:
                    global.type =
                        types_.internInt(sema::mapIntegerWidth(constant.bits, constant.isSigned));
                    break;
                case cinterop::ConstantKind::Float:
                    global.type = types_.internFloat(sema::mapFloatWidth(constant.bits));
                    break;
                case cinterop::ConstantKind::Bool:
                    global.type = types::kBoolType;
                    break;
                case cinterop::ConstantKind::Char:
                    global.type = types::kCharType;
                    break;
                }
                hir::HirLiteral literal;
                literal.type = global.type;
                switch (constant.kind) {
                case cinterop::ConstantKind::Integer:
                case cinterop::ConstantKind::Char:
                    literal.i =
                        static_cast<int64_t>(constant.kind == cinterop::ConstantKind::Char
                                                 ? static_cast<unsigned char>(constant.charValue)
                                                 : constant.integerValue);
                    break;
                case cinterop::ConstantKind::Float:
                    literal.f = constant.floatValue;
                    break;
                case cinterop::ConstantKind::Bool:
                    literal.b = constant.boolValue;
                    break;
                }
                global.init = addExpr(std::move(literal));
                global_const_by_name_.insert(key, global.name);
            }
        }

        for (const auto &decl : module.frontend->declarations()) {
            if (decl.kind != frontend::DeclKind::Variable ||
                decl.bindingKind != frontend::BindingKind::Const || decl.name.empty())
                continue;

            const auto key = internFunctionKey(interner_, module.key, decl.id);
            if (global_const_by_key_.get(key) != nullptr)
                continue;

            const auto name_space   = moduleNamespace(module.key, snapshot_.cacheKey());
            std::string global_name = "_zith_";
            if (!name_space.empty()) {
                global_name += name_space;
                global_name += '.';
            }
            global_name += decl.name;

            auto &global = hir_.addGlobalConst();
            global.name  = interner_.intern(global_name);
            global.type  = lowerType(module_sema->typeOfDecl(decl.id));
            if (decl.initializer) {
                current_module_     = &module;
                current_resolution_ = snapshot_.findResolution(module.key);
                current_types_      = sema_.findTypedMap(module.key);
                global.init         = lowerExpr(decl.initializer);
                current_module_     = nullptr;
                current_resolution_ = nullptr;
                current_types_      = nullptr;
            }
            global_const_by_key_.insert(key, global.name);
        }
    }
    return !diags_.hasErrors();
}

bool HirLowerModern::predeclareFunctions() {
    for (const auto &module_ptr : snapshot_.modules()) {
        const auto &module = *module_ptr;
        auto *module_sema  = sema_.findModuleSema(module.key);
        if (module_sema == nullptr)
            continue;

        for (const auto &decl : module.frontend->declarations()) {
            if (decl.kind != frontend::DeclKind::Function || decl.name.empty())
                continue;
            if (!decl.genericParams.empty())
                continue;

            // Linkage name: `<namespace>.<Owner>.<name>(<param types>)`.  `extern fn`
            // (fixed C ABI) and `main` (needed verbatim by the linker) keep the
            // source name; `= extern <symbol>` aliases to the C linker symbol;
            // everything else is qualified so overloads and same-named
            // functions in different modules get distinct symbols.
            std::string fn_name = decl.externalSymbol.empty() ? decl.name : decl.externalSymbol;
            if (!decl.isExtern && decl.name != "main") {
                if (decl.externalSymbol.empty()) {
                    const auto name_space = moduleNamespace(module.key, snapshot_.cacheKey());
                    std::string qualified;
                    if (!name_space.empty())
                        qualified = name_space + ".";
                    if (!decl.ownerName.empty())
                        qualified += decl.ownerName + ".";
                    if (!decl.parentName.empty())
                        qualified += decl.parentName + "$local.";
                    qualified += decl.name;
                    qualified += frontend::functionSignature(*module.frontend, decl);
                    fn_name = std::move(qualified);
                }
            } else if (!decl.ownerName.empty() && decl.externalSymbol.empty()) {
                fn_name = decl.ownerName + "." + fn_name;
            }

            auto &hir_fn      = hir_.addFn(interner_.intern(fn_name));
            hir_fn.sym_id     = static_cast<symbols::SymId>(functions_.size() + next_sym_id_);
            hir_fn.decl_id    = static_cast<ast::DeclId>(decl.id.value);
            hir_fn.fnSpan     = memory::Span{0, decl.span.start, decl.span.end};
            hir_fn.isVariadic = decl.isVariadic;
            hir_fn.isState    = decl.functionKind == frontend::FunctionKind::State;
            if (!decl.parameters.empty() && decl.parameters.back().isVariadicSlice)
                hir_fn.variadicSliceParam = decl.parameters.size() - 1U;

            bool is_main_void  = false;
            const auto fn_type = module_sema->typeOfDecl(decl.id);
            if (const auto *fn = sema_.typeTable().function(fn_type)) {
                hir_fn.return_type = lowerType(fn->result);
                is_main_void = decl.name == "main" && !decl.isExtern && decl.ownerName.empty() &&
                               decl.visibility != frontend::Visibility::Public &&
                               module.key == snapshot_.rootModuleKey() &&
                               hir_fn.return_type == types::kVoidType;
                if (is_main_void)
                    hir_fn.return_type = types_.internInt(types::IntWidth::I32);
                if (hir_fn.isState) {
                    hir_fn.machineId  = decl.parentScope ? module_sema->stateMachineIdOf(decl)
                                                         : module_sema->stateMachineIdFor(decl);
                    hir_fn.usesTailCC = true;
                    hir_fn.machineReturnType = hir_fn.return_type;
                }
                for (size_t index = 0; index < decl.parameters.size(); ++index) {
                    const auto &parameter = decl.parameters[index];
                    const auto param_type = index < fn->params.size() ? lowerType(fn->params[index])
                                                                      : types::kErrorType;
                    hir_fn.params.push(param_type);
                    hir_fn.param_names.push(interner_.intern(parameter.name));
                }
            } else {
                hir_fn.return_type = types::kVoidType;
                if (hir_fn.isState) {
                    hir_fn.machineId  = decl.parentScope ? module_sema->stateMachineIdOf(decl)
                                                         : module_sema->stateMachineIdFor(decl);
                    hir_fn.usesTailCC = true;
                    hir_fn.machineReturnType = types::kVoidType;
                }
                for (const auto &parameter : decl.parameters) {
                    hir_fn.params.push(types::kErrorType);
                    hir_fn.param_names.push(interner_.intern(parameter.name));
                }
            }

            const auto key = internFunctionKey(interner_, module.key, decl.id);
            function_index_by_key_.insert(key, hir_.getFnCount() - 1U);
            functions_.push_back({key, module_ptr.get(), &decl, nullptr, nullptr, hir_fn.sym_id,
                                  hir_.getFnCount() - 1U, is_main_void});
        }
    }
    if (const auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
        for (size_t index = 0; index < instantiations->instanceCount(); ++index) {
            const auto *instance = instantiations->at(index);
            if (instance != nullptr)
                predeclareInstantiation(instance->module, *instance);
        }
    }
    for (const auto &header : snapshot_.cHeaders()) {
        if (header == nullptr)
            continue;
        for (const auto &foreign : header->functions) {
            auto &hir_fn       = hir_.addFn(interner_.intern(foreign.linkageName));
            hir_fn.sym_id      = static_cast<symbols::SymId>(functions_.size() + next_sym_id_);
            hir_fn.return_type = lowerForeignType(foreign.result);
            hir_fn.isVariadic  = foreign.isVariadic;
            for (const auto &parameter : foreign.parameters) {
                hir_fn.params.push(lowerForeignType(parameter));
                hir_fn.param_names.push({});
            }
            functions_.push_back(
                {0, nullptr, nullptr, &foreign, nullptr, hir_fn.sym_id, hir_.getFnCount() - 1U});
        }
    }
    return true;
}

namespace {

hir::HirOwnership mapHirOwnership(types::OwnershipKind kind) noexcept {
    switch (kind) {
    case types::OwnershipKind::Lend:
        return hir::HirOwnership::Lend;
    case types::OwnershipKind::View:
        return hir::HirOwnership::View;
    case types::OwnershipKind::Unique:
        return hir::HirOwnership::Unique;
    case types::OwnershipKind::Share:
        return hir::HirOwnership::Share;
    case types::OwnershipKind::Belong:
        return hir::HirOwnership::Belong;
    case types::OwnershipKind::Default:
        break;
    }
    return hir::HirOwnership::Default;
}

hir::HirCallEscape mapHirEscape(sema::modern::NraArgEscape escape) noexcept {
    switch (escape) {
    case sema::modern::NraArgEscape::Borrow:
        return hir::HirCallEscape::Borrow;
    case sema::modern::NraArgEscape::Capture:
        return hir::HirCallEscape::Capture;
    case sema::modern::NraArgEscape::Escape:
        return hir::HirCallEscape::Escape;
    case sema::modern::NraArgEscape::Move:
        return hir::HirCallEscape::Move;
    case sema::modern::NraArgEscape::None:
        break;
    }
    return hir::HirCallEscape::None;
}

} // namespace

uint32_t HirLowerModern::alignUp(uint32_t value, uint32_t align) noexcept {
    if (align == 0)
        return value;
    const uint32_t remainder = value % align;
    return remainder == 0 ? value : value + (align - remainder);
}

uint32_t HirLowerModern::lowerTypeSize(types::TypeId type) noexcept {
    switch (types_.kindOf(type)) {
    case types::TypeKind::Bool:
    case types::TypeKind::Char:
        return 1;
    case types::TypeKind::Int: {
        const auto *integer = std::get_if<types::TypeInt>(&types_.lookup(type));
        return integer != nullptr ? (types::intWidthBits(integer->width) + 7U) / 8U : 0U;
    }
    case types::TypeKind::Float: {
        const auto *floating = std::get_if<types::TypeFloat>(&types_.lookup(type));
        if (floating == nullptr)
            return 0U;
        switch (floating->width) {
        case types::FloatWidth::F32:
            return 4U;
        case types::FloatWidth::F64:
            return 8U;
        case types::FloatWidth::F128:
            return 16U;
        }
        return 0U;
    }
    case types::TypeKind::Ptr:
        return 8U;
    case types::TypeKind::Optional: {
        const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(type));
        if (optional == nullptr)
            return 0U;
        if (types_.kindOf(optional->inner) == types::TypeKind::Ptr)
            return 8U;
        const auto inner_size  = lowerTypeSize(optional->inner);
        const auto inner_align = lowerTypeAlign(optional->inner);
        return inner_size == 0U ? 0U : alignUp(alignUp(inner_size, 1U) + 1U, inner_align);
    }
    case types::TypeKind::Failable:
        return 8U;
    case types::TypeKind::Array: {
        const auto *array = std::get_if<types::TypeArray>(&types_.lookup(type));
        return array != nullptr ? lowerTypeSize(array->elem) * array->count : 0U;
    }
    case types::TypeKind::Slice:
        return 16U;
    case types::TypeKind::Enum: {
        const auto *enumeration = std::get_if<types::TypeEnum>(&types_.lookup(type));
        return enumeration != nullptr
                   ? lowerTypeSize(types_.getEnumDef(enumeration->def_id).underlying)
                   : 0U;
    }
    case types::TypeKind::Union: {
        const auto *union_type = std::get_if<types::TypeUnion>(&types_.lookup(type));
        if (union_type == nullptr)
            return 0U;
        const auto *def = types_.lookupUnionDef(union_type->def_id);
        if (def == nullptr)
            return 0U;
        uint32_t max_bytes = 1U;
        uint32_t max_align = 1U;
        for (const auto member : def->members) {
            max_align = std::max(max_align, lowerTypeAlign(member));
            max_bytes = std::max(max_bytes, lowerTypeSize(member));
        }
        if (!def->is_tagged)
            return alignUp(max_bytes, max_align);
        // Tagged unions append the smallest sufficient member-index tag after
        // the aligned payload.
        const auto payload_bytes = alignUp(max_bytes, max_align);
        return alignUp(payload_bytes + tagByteCount(static_cast<uint32_t>(def->members.size())),
                       max_align);
    }
    case types::TypeKind::Struct: {
        const auto *structure = std::get_if<types::TypeStruct>(&types_.lookup(type));
        if (structure == nullptr)
            return 0U;
        const auto &def = types_.getStructDef(structure->def_id);
        uint32_t offset = 0U;
        for (const auto &field : def.fields) {
            const auto align = lowerTypeAlign(field.type);
            if (align == 0U)
                continue;
            offset = alignUp(offset, align);
            offset += lowerTypeSize(field.type);
        }
        return offset;
    }
    case types::TypeKind::Qualified: {
        const auto *qualified = std::get_if<types::TypeQualified>(&types_.lookup(type));
        return qualified != nullptr ? lowerTypeSize(qualified->inner) : 0U;
    }
    case types::TypeKind::Alias: {
        const auto *alias = std::get_if<types::TypeAlias>(&types_.lookup(type));
        return alias != nullptr ? lowerTypeSize(alias->target) : 0U;
    }
    case types::TypeKind::Nominal: {
        const auto *nominal = std::get_if<types::TypeNominal>(&types_.lookup(type));
        return nominal != nullptr ? lowerTypeSize(nominal->target) : 0U;
    }
    default:
        return 0U;
    }
}

uint32_t HirLowerModern::lowerTypeAlign(types::TypeId type) noexcept {
    switch (types_.kindOf(type)) {
    case types::TypeKind::Bool:
    case types::TypeKind::Char:
        return 1U;
    case types::TypeKind::Int: {
        const auto *integer = std::get_if<types::TypeInt>(&types_.lookup(type));
        return integer != nullptr ? ((types::intWidthBits(integer->width) + 7U) / 8U) : 0U;
    }
    case types::TypeKind::Float: {
        const auto *floating = std::get_if<types::TypeFloat>(&types_.lookup(type));
        if (floating == nullptr)
            return 0U;
        switch (floating->width) {
        case types::FloatWidth::F32:
            return 4U;
        case types::FloatWidth::F64:
            return 8U;
        case types::FloatWidth::F128:
            return 16U;
        }
        return 0U;
    }
    case types::TypeKind::Ptr:
    case types::TypeKind::Failable:
        return 8U;
    case types::TypeKind::Optional: {
        const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(type));
        if (optional == nullptr)
            return 0U;
        if (types_.kindOf(optional->inner) == types::TypeKind::Ptr)
            return 8U;
        return lowerTypeAlign(optional->inner);
    }
    case types::TypeKind::Array: {
        const auto *array = std::get_if<types::TypeArray>(&types_.lookup(type));
        return array != nullptr ? lowerTypeAlign(array->elem) : 0U;
    }
    case types::TypeKind::Slice:
        return 8U;
    case types::TypeKind::Enum: {
        const auto *enumeration = std::get_if<types::TypeEnum>(&types_.lookup(type));
        return enumeration != nullptr
                   ? lowerTypeAlign(types_.getEnumDef(enumeration->def_id).underlying)
                   : 0U;
    }
    case types::TypeKind::Union: {
        const auto *union_type = std::get_if<types::TypeUnion>(&types_.lookup(type));
        if (union_type == nullptr)
            return 0U;
        const auto *def = types_.lookupUnionDef(union_type->def_id);
        if (def == nullptr)
            return 0U;
        uint32_t max_align = 1U;
        for (const auto member : def->members)
            max_align = std::max(max_align, lowerTypeAlign(member));
        return max_align;
    }
    case types::TypeKind::Struct: {
        const auto *structure = std::get_if<types::TypeStruct>(&types_.lookup(type));
        if (structure == nullptr)
            return 0U;
        uint32_t max_align = 1U;
        for (const auto &field : types_.getStructDef(structure->def_id).fields)
            max_align = std::max(max_align, lowerTypeAlign(field.type));
        return max_align;
    }
    case types::TypeKind::Qualified: {
        const auto *qualified = std::get_if<types::TypeQualified>(&types_.lookup(type));
        return qualified != nullptr ? lowerTypeAlign(qualified->inner) : 0U;
    }
    case types::TypeKind::Alias: {
        const auto *alias = std::get_if<types::TypeAlias>(&types_.lookup(type));
        return alias != nullptr ? lowerTypeAlign(alias->target) : 0U;
    }
    case types::TypeKind::Nominal: {
        const auto *nominal = std::get_if<types::TypeNominal>(&types_.lookup(type));
        return nominal != nullptr ? lowerTypeAlign(nominal->target) : 0U;
    }
    default:
        return 0U;
    }
}

uint32_t HirLowerModern::tagByteCount(uint32_t member_count) noexcept {
    if (member_count <= 0xFFU)
        return 1U;
    if (member_count <= 0xFFFFU)
        return 2U;
    return 4U;
}

types::TypeId HirLowerModern::tagType(types::TypeIntern &types, uint32_t member_count) noexcept {
    if (member_count <= 0xFFU)
        return types.internInt(types::IntWidth::U8);
    if (member_count <= 0xFFFFU)
        return types.internInt(types::IntWidth::U16);
    return types.internInt(types::IntWidth::U32);
}

types::TypeId HirLowerModern::lowerTagType(types::TypeId type, types::TypeIntern &types,
                                           uint32_t member_count) noexcept {
    const auto *union_type = std::get_if<types::TypeUnion>(&types.lookup(type));
    if (union_type == nullptr)
        return types::kInvalidType;
    const auto *def = types.lookupUnionDef(union_type->def_id);
    if (def == nullptr || !def->is_tagged)
        return types::kInvalidType;
    return tagType(types, member_count);
}

uint32_t HirLowerModern::taggedMemberIndex(types::TypeId union_type,
                                           types::TypeId member) noexcept {
    if (types_.kindOf(union_type) != types::TypeKind::Union)
        return ~0U;
    const auto *union_data = std::get_if<types::TypeUnion>(&types_.lookup(union_type));
    if (union_data == nullptr)
        return ~0U;
    const auto *def = types_.lookupUnionDef(union_data->def_id);
    if (def == nullptr || !def->is_tagged)
        return ~0U;
    uint32_t index = 0;
    for (const auto candidate : def->members) {
        if (candidate == member)
            return index;
        ++index;
    }
    return ~0U;
}

hir::HirExprId HirLowerModern::rebuildTaggedUnion(types::TypeId union_type, hir::HirExprId value,
                                                  uint32_t member_index) {
    const auto *union_data = std::get_if<types::TypeUnion>(&types_.lookup(union_type));
    if (union_data == nullptr)
        return hir::kInvalidHirExpr;
    const auto *def = types_.lookupUnionDef(union_data->def_id);
    if (def == nullptr || !def->is_tagged)
        return hir::kInvalidHirExpr;
    const auto &members = def->members;
    hir::HirUnionCast cast;
    cast.value        = value;
    cast.from         = member_index < members.size() ? members[member_index] : types::kInvalidType;
    cast.to           = union_type;
    cast.member_index = member_index;
    cast.checked      = false;
    return addExpr(std::move(cast));
}

bool HirLowerModern::lowerFunctionBodies() {
    for (auto &function : functions_) {
        if (function.decl != nullptr && function.decl->body && !lowerFunctionBody(function))
            return false;
    }
    return !diags_.hasErrors();
}

void HirLowerModern::predeclareInstantiation(session::ModuleKey module_key,
                                             const comptime::InstantiationInstance &instance) {
    const auto *module      = snapshot_.findModule(module_key);
    const auto *module_sema = sema_.findModuleSema(module_key);
    if (module == nullptr)
        return;
    const auto *decl = findDecl(*module, instance.decl);
    if (module_sema == nullptr || decl == nullptr)
        return;

    const bool has_external_symbol = !decl->externalSymbol.empty();
    auto &hir_fn =
        hir_.addFn(interner_.intern(has_external_symbol ? decl->externalSymbol : instance.mangled));
    hir_fn.sym_id     = static_cast<symbols::SymId>(functions_.size() + next_sym_id_);
    hir_fn.decl_id    = static_cast<ast::DeclId>(decl->id.value);
    hir_fn.fnSpan     = memory::Span{0, decl->span.start, decl->span.end};
    hir_fn.isVariadic = decl->isVariadic;
    hir_fn.isState    = decl->functionKind == frontend::FunctionKind::State;
    if (!decl->parameters.empty() && decl->parameters.back().isVariadicSlice)
        hir_fn.variadicSliceParam = decl->parameters.size() - 1U;

    const auto template_type  = module_sema->typeOfDecl(decl->id);
    const auto *template_fn   = sema_.typeTable().function(template_type);
    const auto *instantiation = sema_.instantiations();
    const auto instance_type  = template_fn != nullptr && instantiation != nullptr
                                    ? instantiation->substituteFunction(*template_fn, instance.args)
                                    : kInvalidTypeId;
    const auto *fn            = sema_.typeTable().function(instance_type);
    if (fn != nullptr) {
        hir_fn.return_type = lowerType(fn->result);
        if (hir_fn.isState) {
            hir_fn.machineId         = module_sema->stateMachineIdOf(*decl);
            hir_fn.usesTailCC        = true;
            hir_fn.machineReturnType = hir_fn.return_type;
        }
        for (size_t index = 0; index < decl->parameters.size() && index < fn->params.size();
             ++index) {
            hir_fn.params.push(lowerType(fn->params[index]));
            hir_fn.param_names.push(interner_.intern(decl->parameters[index].name));
        }
    }

    const auto key = internFunctionKey(interner_, module_key, instance.decl);
    // The first lowered concrete symbol for a generic decl is the canonical
    // target; call-site bindings identify the exact instance, so the key also
    // needs to encode the type-argument tuple when multiple instances exist.
    if (!function_index_by_key_.contains(key))
        function_index_by_key_.insert(key, hir_.getFnCount() - 1U);
    functions_.push_back(
        {key, module, decl, nullptr, &instance, hir_fn.sym_id, hir_.getFnCount() - 1U});
}

bool HirLowerModern::lowerFunctionBody(FunctionInfo &info) {
    current_module_     = info.module;
    current_resolution_ = snapshot_.findResolution(info.module->key);
    current_types_      = sema_.findTypedMap(info.module->key);
    if (current_resolution_ == nullptr || current_types_ == nullptr) {
        diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                      "missing semantic data for module '" + info.module->key + "'", {});
        return false;
    }

    current_fn_                  = &hir_.getFn(info.hir_index);
    current_instantiation_       = info.instance != nullptr ? sema_.instantiations() : nullptr;
    current_instance_            = info.instance;
    current_fn_return_sema_type_ = sema::modern::kInvalidTypeId;
    current_fn_decl_             = info.decl;
    if (const auto *module_sema = sema_.findModuleSema(info.module->key);
        module_sema != nullptr && info.decl != nullptr) {
        sema::modern::TypeId fn_sema_type = module_sema->typeOfDecl(info.decl->id);
        if (const auto *fn = sema_.typeTable().function(fn_sema_type))
            current_fn_return_sema_type_ = fn->result;
    }
    info_decl_parent_scope_ = info.decl != nullptr ? info.decl->parentScope : frontend::ScopeId{};
    current_fn_is_state_ =
        info.decl != nullptr && info.decl->functionKind == frontend::FunctionKind::State;
    current_state_machine_id_ = current_fn_is_state_ ? current_fn_->machineId : 0;

    if (nra_ != nullptr && info.module != nullptr && info.decl != nullptr) {
        const auto *fn_fact = nra_->functionFact(info.module->key, info.decl->id);
        if (fn_fact != nullptr && fn_fact->hasResidual() && fn_fact->allReturnsParameter &&
            fn_fact->parameterIndex != ~0U) {
            auto &attrs          = hir_.attrs().fn(info.hir_index);
            attrs.returnConsumed = hir::HirConsumedState::NonConsumed;
            attrs.noAlias        = true;
        }
    }

    current_block_ = 0;
    next_slot_     = 0;
    loop_stack_.clear();
    narrowing_stack_.clear();
    local_slots_.clear();
    local_slots_.resize(1U);
    current_for_in_binding_stmt_  = {};
    current_for_in_binding_local_ = {};
    const bool is_main_void       = info.decl->name == "main" && info.forced_main_return_i32 &&
                              info.module != nullptr &&
                              info.module->key == snapshot_.rootModuleKey();
    current_main_void_ = is_main_void;

    current_fn_->blocks.emplace(arena_);
    current_fn_->blocks[0].insts = memory::DynArray<hir::HirExprId>(arena_);

    for (size_t index = 0; index < info.decl->parameters.size(); ++index) {
        const auto &parameter = info.decl->parameters[index];
        const auto slot       = localSlot(parameter.id);
        current_fn_->param_slots.push(slot);
        // The slot id is read by codegen from param_slots; publish attrs for
        // parameters even when the function body never names them, because the
        // ABI-level ownership decision depends on the parameter itself.
        if (nra_ != nullptr) {
            const auto *fact = nra_->localFact(parameter.id);
            if (fact != nullptr && fact->hasResidual()) {
                auto &attrs     = hir_.attrs().slot(slot);
                attrs.ownership = mapHirOwnership(fact->ownership);
                attrs.nonNull   = fact->nonNull;
            }
        }
        current_fn_->blocks[0].insts.push(emitSlotAlloca(slot, current_fn_->params[index]));

        hir::HirVar param_var;
        param_var.name        = current_fn_->param_names[index];
        param_var.version     = 0;
        const auto param_expr = addExpr(std::move(param_var));
        current_fn_->blocks[0].insts.push(emitSlotStore(slot, param_expr));
    }

    const auto body_expr = lowerExpr(info.decl->body);
    if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr) {
        hir::HirRet ret;
        if (current_main_void_) {
            hir::HirLiteral zero;
            zero.type = types_.internInt(types::IntWidth::I32);
            zero.i    = 0;
            ret.value = addExpr(std::move(zero));
        } else if (current_fn_->return_type != types::kVoidType &&
                   body_expr != hir::kInvalidHirExpr) {
            if (types_.kindOf(current_fn_->return_type) == types::TypeKind::Optional &&
                info.decl->body) {
                const auto val_sema_type = semaTypeOfExpr(info.decl->body);
                if (current_fn_return_sema_type_ != sema::modern::kInvalidTypeId &&
                    val_sema_type != sema::modern::kInvalidTypeId)
                    ret.value = lowerCoerceToOptionalDepth(current_fn_->return_type,
                                                           current_fn_return_sema_type_,
                                                           val_sema_type, body_expr);
                else if (sema_.typeTable().kindOf(val_sema_type) != TypeKind::Optional)
                    ret.value = lowerCoerceToOptional(current_fn_->return_type, body_expr);
                else {
                    ret.value = body_expr;
                }
            } else {
                ret.value = body_expr;
            }
            if (current_fn_return_sema_type_ != sema::modern::kInvalidTypeId)
                ret.value = lowerCoerceToDyn(current_fn_return_sema_type_, info.decl->body,
                                             ret.value, current_fn_return_sema_type_);
            if (current_fn_return_sema_type_ != sema::modern::kInvalidTypeId)
                ret.value =
                    lowerCoerceToOpaque(current_fn_return_sema_type_, info.decl->body, ret.value);
            ret.value = lowerCoerceToTarget(current_fn_->return_type, info.decl->body, ret.value);
        }
        current_fn_->blocks[current_block_].terminator = addExpr(std::move(ret));
    }

    current_module_              = nullptr;
    current_resolution_          = nullptr;
    current_types_               = nullptr;
    current_instantiation_       = nullptr;
    current_instance_            = nullptr;
    current_fn_return_sema_type_ = sema::modern::kInvalidTypeId;
    info_decl_parent_scope_      = {};
    current_fn_                  = nullptr;
    current_fn_is_state_         = false;
    current_state_machine_id_    = 0;
    current_main_void_           = false;
    return true;
}

types::TypeId HirLowerModern::lowerType(sema::modern::TypeId type) {
    if (!type)
        return types::kErrorType;
    // Nominal placeholders must lower to the completed type, not to Unknown.
    type = sema_.typeTable().canonical(type);
    if (const auto *cached = lowered_types_.get(type.intern_seq))
        return *cached;

    types::TypeId lowered = types::kErrorType;
    switch (sema_.typeTable().kindOf(type)) {
    case TypeKind::Error:
    case TypeKind::Invalid:
        lowered = types::kErrorType;
        break;
    case TypeKind::Void:
        lowered = types::kVoidType;
        break;
    case TypeKind::Never:
        lowered = types::kNeverType;
        break;
    case TypeKind::Bool:
        lowered = types::kBoolType;
        break;
    case TypeKind::Char:
        lowered = types::kCharType;
        break;
    case TypeKind::Integer: {
        const auto *integer = sema_.typeTable().integer(type);
        lowered             = integer != nullptr
                                  ? types_.internInt(sema::mapIntegerWidth(integer->bits, integer->isSigned))
                                  : types::kErrorType;
        break;
    }
    case TypeKind::Float: {
        const auto *floating = sema_.typeTable().float_kind(type);
        lowered = floating != nullptr ? types_.internFloat(sema::mapFloatWidth(floating->bits))
                                      : types::kErrorType;
        break;
    }
    case TypeKind::String:
        lowered = types_.internPtr(types::kCharType);
        break;
    case TypeKind::Pointer: {
        const auto *pointer = sema_.typeTable().pointer(type);
        lowered =
            pointer != nullptr ? types_.internPtr(lowerType(pointer->pointee)) : types::kErrorType;
        break;
    }
    case TypeKind::Optional: {
        const auto *optional = sema_.typeTable().optional(type);
        lowered = optional != nullptr ? types_.internOptional(lowerType(optional->inner))
                                      : types::kErrorType;
        break;
    }
    case TypeKind::Array: {
        const auto *array = sema_.typeTable().array(type);
        lowered           = array != nullptr ? types_.internArray(lowerType(array->element),
                                                                  static_cast<uint32_t>(array->size))
                                             : types::kErrorType;
        break;
    }
    case TypeKind::Function:
    case TypeKind::State: {
        const auto *fn = sema_.typeTable().function(type);
        if (fn == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        memory::DynArray<types::TypeId> params(arena_);
        params.reserve(fn->params.size());
        for (const auto param : fn->params)
            params.push(lowerType(param));
        lowered = types_.internFn(params, lowerType(fn->result));
        break;
    }
    case TypeKind::Struct: {
        const auto *structure = sema_.typeTable().struct_type(type);
        if (structure == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        // Register the name (done by the named-type not found path).
        lowered = types_.registerNamedType(structure->name, types::TypeKind::Struct);
        // Register the name (done above) before lowering field types so self-referential
        // structs (`next: *Node`) terminate. Fields are copied once, on first lowering.
        if (types_.fieldCount(lowered) == 0U && structure->fields.size() != 0U) {
            lowered_types_.insert(type.intern_seq, lowered);
            for (size_t index = 0; index < structure->fields.size(); ++index) {
                const auto field_name = index < structure->field_names.size()
                                            ? structure->field_names[index]
                                            : std::string_view{};
                types_.addField(lowered, field_name, lowerType(structure->fields[index]));
            }
        }
        break;
    }
    case TypeKind::Enum: {
        const auto *enumeration = sema_.typeTable().enum_type(type);
        if (enumeration == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        // Register the named enum with its underlying type and variants so codegen can
        // lower it to the underlying integer instead of `void` (a plain registerNamedType
        // would leave the underlying as kErrorType).
        lowered = types_.defineEnum(enumeration->name, lowerType(enumeration->underlying));
        for (size_t i = 0; i < enumeration->variant_names.size(); ++i)
            types_.addEnumVariant(lowered, enumeration->variant_names[i],
                                  enumeration->discriminants[i]);
        break;
    }
    case TypeKind::Union: {
        const auto *union_type = sema_.typeTable().union_type(type);
        if (union_type == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        lowered                   = types_.defineUnion(union_type->name, union_type->is_tagged);
        const auto *lowered_union = std::get_if<types::TypeUnion>(&types_.lookup(lowered));
        const auto *def =
            lowered_union != nullptr ? types_.lookupUnionDef(lowered_union->def_id) : nullptr;
        if (def != nullptr && def->members.size() == 0U) {
            lowered_types_.insert(type.intern_seq, lowered);
            for (const auto member : union_type->members)
                types_.addUnionMember(lowered, lowerType(member));
        }
        break;
    }
    case TypeKind::Trait:
    case TypeKind::TypeVar:
    case TypeKind::Unknown:
        lowered = types_.internUnknown();
        break;
    case TypeKind::GenericParam: {
        uint32_t decl_id   = 0;
        uint32_t param_idx = 0;
        sema_.typeTable().genericParamOrigin(type, &decl_id, &param_idx);
        lowered = types_.internGenericParam(decl_id, param_idx);
        break;
    }
    case TypeKind::Incomplete: {
        const auto *incomplete = sema_.typeTable().incomplete(type);
        if (incomplete == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        memory::DynArray<types::TypeId> args(arena_);
        args.reserve(incomplete->args.size());
        for (const auto arg : incomplete->args)
            args.push(lowerType(arg));
        lowered = types_.internIncomplete(lowerType(incomplete->base), args);
        break;
    }
    case TypeKind::Sum: {
        const auto *sum = sema_.typeTable().sum(type);
        if (sum == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        memory::DynArray<types::TypeId> members(arena_);
        members.reserve(sum->members.size());
        for (const auto member : sum->members)
            members.push(lowerType(member));
        lowered = types_.internSum(members);
        break;
    }
    case TypeKind::Slice: {
        const auto *slice = sema_.typeTable().slice(type);
        lowered =
            slice != nullptr ? types_.internSlice(lowerType(slice->element)) : types::kErrorType;
        break;
    }
    case TypeKind::Failable: {
        const auto *failable = sema_.typeTable().failable(type);
        lowered = failable != nullptr ? types_.internFailable(lowerType(failable->inner))
                                      : types::kErrorType;
        break;
    }
    case TypeKind::Pack: {
        const auto *pack = sema_.typeTable().pack(type);
        if (pack == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        memory::DynArray<types::TypeId> members(arena_);
        memory::DynArray<memory::InternedId> names(arena_);
        members.reserve(pack->members.size());
        names.reserve(pack->names.size());
        for (const auto member : pack->members)
            members.push(lowerType(member));
        for (const auto name : pack->names)
            names.push(interner_.intern(name));
        lowered = types_.internPack(members, names);
        break;
    }
    case TypeKind::Dyn: {
        const auto *dyn = sema_.typeTable().dyn_type(type);
        lowered = dyn != nullptr ? types_.internDyn(lowerType(dyn->target), dyn->method_count)
                                 : types::kErrorType;
        break;
    }
    case TypeKind::Opaque: {
        lowered = types_.internOpaqueTagged();
        break;
    }
    case TypeKind::Alias: {
        const auto *alias = sema_.typeTable().alias(type);
        lowered           = alias != nullptr ? lowerType(alias->target) : types::kErrorType;
        break;
    }
    case TypeKind::Nominal: {
        const auto *nom = sema_.typeTable().nominal(type);
        if (nom == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        lowered = types_.defineStruct(nom->name);
        if (types_.fieldCount(lowered) == 0U) {
            lowered_types_.insert(type.intern_seq, lowered);
            types_.addField(lowered, "", lowerType(nom->target));
        }
        break;
    }
    case TypeKind::Qualified: {
        // HIR and codegen do not represent ownership: strip to the inner type.
        const auto *qual = sema_.typeTable().qualified(type);
        lowered          = qual != nullptr ? lowerType(qual->inner) : types::kErrorType;
        break;
    }
    }

    lowered_types_.insert(type.intern_seq, lowered);
    return lowered;
}

sema::modern::TypeId HirLowerModern::lowerTypeExprConcrete(frontend::TypeExprId id) {
    if (!id || current_module_ == nullptr || current_module_->frontend == nullptr)
        return sema::modern::kInvalidTypeId;
    sema::modern::TypeId lowered = sema_.typeTable().lowerTypeExpr(*current_module_->frontend, id);
    if (!lowered && current_fn_decl_ != nullptr &&
        id.value <= current_module_->frontend->typeExpressions().size()) {
        const auto &type_expr = current_module_->frontend->typeExpressions()[id.value - 1U];
        if (type_expr.kind == frontend::TypeExprKind::Name && type_expr.arguments.empty()) {
            const auto findGenericParam = [&](const frontend::Declaration &decl) {
                for (size_t i = 0; i < decl.genericParams.size(); ++i) {
                    if (decl.genericParams[i].name == type_expr.name)
                        return sema_.typeTable().internGenericParam(decl.id.value,
                                                                    static_cast<uint32_t>(i));
                }
                return sema::modern::kInvalidTypeId;
            };
            if (!current_fn_decl_->ownerName.empty()) {
                for (const auto &decl : current_module_->frontend->declarations()) {
                    if (decl.name == current_fn_decl_->ownerName &&
                        decl.id.value != current_fn_decl_->id.value) {
                        lowered = findGenericParam(decl);
                        break;
                    }
                }
            }
            if (!lowered)
                lowered = findGenericParam(*current_fn_decl_);
        }
    }
    if (lowered && current_instantiation_ != nullptr && current_instance_ != nullptr) {
        lowered = current_instantiation_->substituteType(lowered, current_instance_->args);
    }
    return lowered;
}

types::TypeId HirLowerModern::lowerForeignType(const cinterop::Type &type) {
    switch (type.kind) {
    case cinterop::TypeKind::Void:
        return types::kVoidType;
    case cinterop::TypeKind::Bool:
        return types::kBoolType;
    case cinterop::TypeKind::Integer:
        if (type.isChar)
            return types::kCharType;
        return types_.internInt(sema::mapIntegerWidth(type.bits, type.isSigned));
    case cinterop::TypeKind::Float:
        return types_.internFloat(sema::mapFloatWidth(type.bits));
    case cinterop::TypeKind::Pointer: {
        // Mirrors `PerModuleSema::lowerForeignType`: a C pointer is `?*T`, which the
        // niche layout emits as the bare pointer.
        const types::TypeId pointee =
            type.pointee ? lowerForeignType(*type.pointee) : types::kErrorType;
        return types_.internOptional(types_.internPtr(pointee));
    }
    case cinterop::TypeKind::Record:
        return types_.registerNamedType(type.name, types::TypeKind::Struct);
    case cinterop::TypeKind::Enum:
        return types_.registerNamedType(type.name, types::TypeKind::Enum);
    }
    return types::kErrorType;
}

types::TypeId HirLowerModern::typeOfExpr(frontend::ExprId id) {
    if (!id || current_types_ == nullptr)
        return types::kErrorType;
    const auto *type = current_types_->exprTypes.get(id.value);
    if (type == nullptr)
        return types::kErrorType;
    const sema::modern::TypeId sema_type =
        current_instantiation_ != nullptr && current_instance_ != nullptr
            ? current_instantiation_->substituteType(*type, current_instance_->args)
            : *type;
    return lowerType(sema_type);
}

types::TypeId HirLowerModern::typeOfLocal(frontend::LocalId id) {
    if (!id || current_types_ == nullptr)
        return types::kErrorType;
    const auto *type = current_types_->localTypes.get(id.value);
    if (type == nullptr)
        return types::kErrorType;
    const sema::modern::TypeId sema_type =
        current_instantiation_ != nullptr && current_instance_ != nullptr
            ? current_instantiation_->substituteType(*type, current_instance_->args)
            : *type;
    return lowerType(sema_type);
}

sema::modern::TypeId HirLowerModern::semaTypeOfLocal(frontend::LocalId id) {
    if (!id || current_types_ == nullptr)
        return kInvalidTypeId;
    const auto *type = current_types_->localTypes.get(id.value);
    if (type == nullptr)
        return kInvalidTypeId;
    return current_instantiation_ != nullptr && current_instance_ != nullptr
               ? current_instantiation_->substituteType(*type, current_instance_->args)
               : *type;
}

sema::modern::TypeId HirLowerModern::semaTypeOfExpr(frontend::ExprId id) {
    if (!id || current_types_ == nullptr)
        return kInvalidTypeId;
    const auto *sema_id_ptr = current_types_->exprTypes.get(id.value);
    if (!sema_id_ptr)
        return kInvalidTypeId;
    return current_instantiation_ != nullptr && current_instance_ != nullptr
               ? current_instantiation_->substituteType(*sema_id_ptr, current_instance_->args)
               : *sema_id_ptr;
}

const frontend::Declaration *HirLowerModern::findDecl(const session::ModuleArtifact &module,
                                                      frontend::DeclId id) const noexcept {
    if (!id || module.frontend == nullptr || id.value > module.frontend->declarations().size())
        return nullptr;
    return &module.frontend->declarations()[id.value - 1U];
}

const PerModuleSema::CallTarget *
HirLowerModern::overloadTarget(frontend::ExprId callee) const noexcept {
    if (current_module_ == nullptr)
        return nullptr;
    const auto *module_sema = sema_.findModuleSema(current_module_->key);
    return module_sema == nullptr ? nullptr : module_sema->resolvedCallTarget(callee);
}

const frontend::Declaration *
HirLowerModern::methodDeclFromTarget(frontend::ExprId callee,
                                     const session::ModuleArtifact **module_out) const noexcept {
    const auto *target = overloadTarget(callee);
    if (target == nullptr)
        return nullptr;
    const auto *module = snapshot_.findModule(target->module);
    if (module == nullptr)
        return nullptr;
    const auto *decl = findDecl(*module, target->decl);
    if (decl == nullptr || decl->kind != frontend::DeclKind::Function)
        return nullptr;
    if (module_out != nullptr)
        *module_out = module;
    return decl;
}

hir::HirExprId HirLowerModern::lowerVisibleDefault(const session::ModuleArtifact &module,
                                                   const comptime::InstantiationInstance *instance,
                                                   frontend::ExprId default_id) {
    if (!default_id || module.frontend == nullptr ||
        default_id.value > module.frontend->expressions().size())
        return hir::kInvalidHirExpr;
    const session::ModuleResolution *decl_resolution = snapshot_.findResolution(module.key);
    const TypedMap *decl_types                       = sema_.findTypedMap(module.key);
    if (decl_resolution == nullptr || decl_types == nullptr)
        return hir::kInvalidHirExpr;

    const session::ModuleArtifact *saved_module           = current_module_;
    const session::ModuleResolution *saved_resolution     = current_resolution_;
    const TypedMap *saved_types                           = current_types_;
    const comptime::GenericInstantiationPass *saved_inst  = current_instantiation_;
    const comptime::InstantiationInstance *saved_instance = current_instance_;
    current_module_                                       = &module;
    current_resolution_                                   = decl_resolution;
    current_types_                                        = decl_types;
    current_instantiation_ = instance != nullptr ? sema_.instantiations() : nullptr;
    current_instance_      = instance;
    const auto value       = lowerExpr(default_id);
    current_module_        = saved_module;
    current_resolution_    = saved_resolution;
    current_types_         = saved_types;
    current_instantiation_ = saved_inst;
    current_instance_      = saved_instance;
    return value;
}

hir::HirExprId HirLowerModern::lowerDefaultWithTarget(
    const session::ModuleArtifact &module, const comptime::InstantiationInstance *instance,
    frontend::ExprId default_id, sema::modern::TypeId target_sema) {
    const session::ModuleResolution *decl_resolution = snapshot_.findResolution(module.key);
    const TypedMap *decl_types                       = sema_.findTypedMap(module.key);
    if (!default_id || module.frontend == nullptr ||
        default_id.value > module.frontend->expressions().size() || decl_resolution == nullptr ||
        decl_types == nullptr)
        return hir::kInvalidHirExpr;

    const session::ModuleArtifact *saved_module           = current_module_;
    const session::ModuleResolution *saved_resolution     = current_resolution_;
    const TypedMap *saved_types                           = current_types_;
    const comptime::GenericInstantiationPass *saved_inst  = current_instantiation_;
    const comptime::InstantiationInstance *saved_instance = current_instance_;
    current_module_                                       = &module;
    current_resolution_                                   = decl_resolution;
    current_types_                                        = decl_types;
    current_instantiation_ = instance != nullptr ? sema_.instantiations() : nullptr;
    current_instance_      = instance;
    const auto value       = current_types_ != nullptr
                                 ? lowerCoerceToOpaque(target_sema, default_id, lowerExpr(default_id))
                                 : hir::kInvalidHirExpr;
    current_module_        = saved_module;
    current_resolution_    = saved_resolution;
    current_types_         = saved_types;
    current_instantiation_ = saved_inst;
    current_instance_      = saved_instance;
    return value;
}

const session::ResolvedName *HirLowerModern::findResolvedExpr(frontend::ExprId id) const noexcept {
    if (current_module_ == nullptr || current_resolution_ == nullptr || !id ||
        id.value > current_module_->frontend->expressions().size()) {
        return nullptr;
    }

    return session::lookupExprResolution(*current_resolution_, id);
}

const frontend::Declaration *
HirLowerModern::resolvedFunctionDecl(const session::ResolvedName &resolved,
                                     const session::ModuleArtifact **module_out) const noexcept {
    const session::ModuleArtifact *module = current_module_;
    frontend::DeclId decl                 = resolved.declaration;
    if (!resolved.target.module.empty()) {
        module = snapshot_.findModule(resolved.target.module);
        if (!decl && resolved.target.localSymbol)
            decl = frontend::DeclId{resolved.target.localSymbol.value};
    }
    if (module_out != nullptr)
        *module_out = module;
    if (module == nullptr)
        return nullptr;
    return findDecl(*module, decl);
}

symbols::SymId
HirLowerModern::resolvedFunctionSym(const session::ResolvedName &resolved) const noexcept {
    if (resolved.foreignFunction != nullptr) {
        for (const auto &function : functions_) {
            if (function.foreign == resolved.foreignFunction)
                return function.sym_id;
        }
        return symbols::kInvalidSym;
    }
    const session::ModuleArtifact *module = nullptr;
    const auto *decl                      = resolvedFunctionDecl(resolved, &module);
    if (decl == nullptr || module == nullptr)
        return symbols::kInvalidSym;
    const auto key = internFunctionKey(interner_, module->key, decl->id);
    if (const auto *function_index = function_index_by_key_.get(key))
        return functions_[*function_index].sym_id;
    return symbols::kInvalidSym;
}

hir::HirSlotId HirLowerModern::localSlot(frontend::LocalId id) {
    if (!id)
        return hir::kInvalidHirSlot;
    if (id.value >= local_slots_.size())
        local_slots_.resize(id.value + 1U, hir::kInvalidHirSlot);
    if (local_slots_[id.value] == hir::kInvalidHirSlot) {
        local_slots_[id.value] = next_slot_++;
        if (nra_ != nullptr) {
            const auto *fact = nra_->localFact(id);
            if (fact != nullptr && fact->hasResidual()) {
                auto &attrs     = hir_.attrs().slot(local_slots_[id.value]);
                attrs.ownership = mapHirOwnership(fact->ownership);
                attrs.nonNull   = fact->nonNull;
                attrs.consumed  = fact->knownAlive ? hir::HirConsumedState::NonConsumed
                                                   : hir::HirConsumedState::Consumed;
            }
        }
    }
    return local_slots_[id.value];
}

hir::HirExprId HirLowerModern::lowerExpr(frontend::ExprId id) {
    if (!id || current_module_ == nullptr ||
        id.value > current_module_->frontend->expressions().size())
        return hir::kInvalidHirExpr;

    const auto &expr = current_module_->frontend->expressions()[id.value - 1U];
    const auto type  = typeOfExpr(id);
    switch (expr.kind) {
    case frontend::ExprKind::OwnershipCoerce:
        return expr.operands.empty() ? hir::kInvalidHirExpr : lowerExpr(expr.operands[0]);
    case frontend::ExprKind::Literal: {
        // Implicit `T -> opaque` coercions record the concrete source type but
        // sema also types the whole value expression as `opaque`. The HIR
        // literal must keep the concrete type so it can be spilled into the
        // erased payload before wrapping.
        types::TypeId literal_type = type;
        if (current_types_ != nullptr) {
            if (const auto *source = current_types_->opaqueSourceTypes.get(id.value)) {
                const sema::modern::TypeId source_sema =
                    current_instantiation_ != nullptr && current_instance_ != nullptr
                        ? current_instantiation_->substituteType(*source, current_instance_->args)
                        : *source;
                const types::TypeId lowered = lowerType(source_sema);
                if (lowered != types::kErrorType && lowered != types::kInvalidType)
                    literal_type = lowered;
            }
        }
        return lowerLiteral(expr, literal_type);
    }
    case frontend::ExprKind::Name:
        return lowerName(expr);
    case frontend::ExprKind::Unary:
        return lowerUnary(expr, type);
    case frontend::ExprKind::Binary:
        return lowerBinary(expr, type);
    case frontend::ExprKind::Call:
        return lowerCall(expr);
    case frontend::ExprKind::DockCall:
        // `dock` is a plain call expression whose result carries the machine
        // return type; sema already resolved the target state declaration.
        return lowerCall(expr);
    case frontend::ExprKind::Block:
        return lowerBlock(expr);
    case frontend::ExprKind::If:
        return lowerIf(expr, type);
    case frontend::ExprKind::When:
        return lowerWhen(expr, type);
    case frontend::ExprKind::Range:
        return hir::kInvalidHirExpr;
    case frontend::ExprKind::While:
        return lowerWhile(expr);
    case frontend::ExprKind::For:
        return lowerFor(expr);
    case frontend::ExprKind::ForIn:
        return lowerForIn(expr);
    case frontend::ExprKind::Assign:
        return lowerAssign(expr, type);
    case frontend::ExprKind::OptionalProp:
        return lowerOptionalProp(expr, type);
    case frontend::ExprKind::Index:
        return lowerIndex(expr, type);
    case frontend::ExprKind::SliceRange:
        return lowerSliceRange(expr, type);
    case frontend::ExprKind::Field:
        return lowerField(expr, type);
    case frontend::ExprKind::Arrow:
        return lowerArrow(expr, type);
    case frontend::ExprKind::StructLiteral:
        return lowerStructLiteral(expr, type);
    case frontend::ExprKind::PackLiteral:
        return lowerPackLiteral(expr, type);
    case frontend::ExprKind::ArrayLiteral:
        return lowerArrayLiteral(expr, type);
    case frontend::ExprKind::Cast:
        return lowerCast(expr, type);
    case frontend::ExprKind::IsNull:
        return lowerIsNull(expr);
    case frontend::ExprKind::IsType:
        return lowerIsType(expr);
    case frontend::ExprKind::LayoutIntrinsic:
        return lowerLayoutIntrinsic(expr);
    case frontend::ExprKind::MacroCall: {
        // Transparent: delegate to expansion.
        if (expr.expansion)
            return lowerExpr(expr.expansion);
        return hir::kInvalidHirExpr;
    }
    case frontend::ExprKind::Return:
    case frontend::ExprKind::Placeholder:
    case frontend::ExprKind::Error:
        return hir::kInvalidHirExpr;
    }
    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerLiteral(const frontend::Expression &expr,
                                            const types::TypeId type) {
    // null literal maps to HirMakeNone when the target type is optional
    if (expr.text == "null" && types_.kindOf(type) == types::TypeKind::Optional)
        return lowerMakeNone(type);
    hir::HirLiteral literal{};
    literal.type = type;
    switch (types_.kindOf(type)) {
    case types::TypeKind::Bool:
        literal.b = expr.text == "true";
        break;
    case types::TypeKind::Float:
        literal.f = std::strtod(expr.text.c_str(), nullptr);
        if (const auto *float_t = std::get_if<types::TypeFloat>(&types_.lookup(type));
            float_t != nullptr) {
            switch (float_t->width) {
            case types::FloatWidth::F32:
                literal.f = static_cast<double>(static_cast<float>(literal.f));
                break;
            default:
                break;
            }
        }
        break;
    case types::TypeKind::Char: {
        if (expr.text.size() < 3U || expr.text.front() != '\'' || expr.text.back() != '\'')
            return hir::kInvalidHirExpr;
        std::string decoded;
        const std::string_view body(expr.text.data() + 1U, expr.text.size() - 2U);
        if (!decodeEscapes(body, decoded) || decoded.size() != 1U) {
            if (current_module_ != nullptr) {
                diags_.report(
                    diagnostics::Severity::Error, diagnostics::err::InvalidEscape,
                    "invalid escape sequence in char literal",
                    memory::Span{current_module_->fileId, expr.span.start, expr.span.end});
            }
            return hir::kInvalidHirExpr;
        }
        literal.i = static_cast<unsigned char>(decoded.front());
        break;
    }
    case types::TypeKind::Ptr: {
        auto text = std::string_view(expr.text);
        if (text.size() >= 2U && text.front() == '"' && text.back() == '"') {
            std::string decoded;
            if (!decodeEscapes(std::string_view(text.data() + 1U, text.size() - 2U), decoded)) {
                if (current_module_ != nullptr) {
                    diags_.report(
                        diagnostics::Severity::Error, diagnostics::err::InvalidEscape,
                        "invalid escape sequence in string literal",
                        memory::Span{current_module_->fileId, expr.span.start, expr.span.end});
                }
                return hir::kInvalidHirExpr;
            }
            literal.str_val = interner_.intern(decoded);
        } else {
            literal.str_val = interner_.intern(text);
        }
        break;
    }
    default:
        // Honour the radix prefix the lexer accepted (`0x`, `0c`, `0b`). Sema has already
        // rejected literals that do not fit 64 bits, so Overflow here leaves the value at 0.
        (void)support::parseIntegerLiteral(expr.text, literal.i);
        break;
    }
    return addExpr(std::move(literal));
}

hir::HirExprId HirLowerModern::lowerName(const frontend::Expression &expr) {
    // Prefer the per-expression resolution: it is keyed by span, so it cannot
    // pick up a same-named binding from another function.
    if (const auto *resolved = findResolvedExpr(expr.id)) {
        if (resolved->local) {
            const auto slot      = localSlot(resolved->local);
            const auto local_ty  = typeOfLocal(resolved->local);
            const auto expr_type = typeOfExpr(expr.id);
            for (auto it = narrowing_stack_.rbegin(); it != narrowing_stack_.rend(); ++it) {
                if (it->local == resolved->local) {
                    if (it->optionalPayload) {
                        // The slot still stores `?T`; extract field 0 so reads
                        // after `is null` use the payload type.
                        return addExpr(hir::HirField{addExpr(hir::HirSlotAddr{slot, local_ty}), 0U,
                                                     it->type, local_ty});
                    }
                    if (it->opaquePayload) {
                        hir::HirOpaqueCast cast;
                        cast.value       = emitSlotLoad(slot, local_ty);
                        cast.from        = local_ty;
                        cast.to          = it->type;
                        cast.opaque_type = local_ty;
                        cast.result_type = it->type;
                        cast.type_id     = stableConcreteTypeId(it->type);
                        cast.checked     = false;
                        return addExpr(std::move(cast));
                    }
                    const auto narrowed = emitSlotLoad(slot, it->type);
                    if (types_.kindOf(local_ty) == types::TypeKind::Union &&
                        types_.kindOf(it->type) != types::TypeKind::Union) {
                        hir::HirUnionCast cast;
                        cast.value        = narrowed;
                        cast.from         = local_ty;
                        cast.to           = it->type;
                        cast.member_index = taggedMemberIndex(local_ty, it->type);
                        cast.checked      = false;
                        return addExpr(std::move(cast));
                    }
                    return narrowed;
                }
            }
            const auto *local_union = types_.kindOf(local_ty) == types::TypeKind::Union
                                          ? std::get_if<types::TypeUnion>(&types_.lookup(local_ty))
                                          : nullptr;
            const auto *local_union_def =
                local_union != nullptr ? types_.lookupUnionDef(local_union->def_id) : nullptr;
            if (local_union_def != nullptr && local_union_def->is_tagged && expr_type != local_ty) {
                hir::HirUnionCast cast;
                cast.value        = emitSlotLoad(slot, local_ty);
                cast.from         = local_ty;
                cast.to           = expr_type;
                cast.member_index = taggedMemberIndex(local_ty, expr_type);
                cast.checked      = false;
                return addExpr(std::move(cast));
            }
            return emitSlotLoad(slot, local_ty);
        }
        if (resolved->declKind == frontend::DeclKind::Variable &&
            resolved->bindingKind == frontend::BindingKind::Const) {
            const session::ModuleArtifact *decl_module = current_module_;
            frontend::DeclId decl_id                   = resolved->declaration;
            if (!resolved->target.module.empty()) {
                decl_module = snapshot_.findModule(resolved->target.module);
                if (!decl_id && resolved->target.localSymbol)
                    decl_id = frontend::DeclId{resolved->target.localSymbol.value};
            }
            if (decl_module != nullptr && decl_id &&
                decl_id.value <= decl_module->frontend->declarations().size()) {
                const auto key = internFunctionKey(interner_, decl_module->key, decl_id);
                if (const auto *global_name = global_const_by_key_.get(key)) {
                    hir::HirGlobalConstLoad load;
                    load.name = *global_name;
                    load.type = typeOfExpr(expr.id);
                    return addExpr(std::move(load));
                }
            }
        }
        if (resolved->foreignConstant != nullptr) {
            const auto key = interner_.intern(resolved->foreignConstant->name);
            if (const auto *global_name = global_const_by_name_.get(key)) {
                hir::HirGlobalConstLoad load;
                load.name = *global_name;
                load.type = typeOfExpr(expr.id);
                return addExpr(std::move(load));
            }
        }
        if (resolved->foreignFunction != nullptr) {
            hir::HirVar var;
            var.name    = interner_.intern(resolved->foreignFunction->linkageName);
            var.version = 0;
            return addExpr(std::move(var));
        }
        const session::ModuleArtifact *decl_module = nullptr;
        if (const auto *decl = resolvedFunctionDecl(*resolved, &decl_module)) {
            hir::HirVar var;
            // Use the predeclared (qualified) linkage name so the reference and the
            // definition agree.
            var.name = interner_.intern(decl->name);
            if (decl_module != nullptr) {
                const auto key = internFunctionKey(interner_, decl_module->key, decl->id);
                if (const auto *function_index = function_index_by_key_.get(key))
                    var.name = hir_.getFn(*function_index).name;
            }
            var.version = 0;
            return addExpr(std::move(var));
        }
    }

    if (current_resolution_ != nullptr && current_module_ != nullptr &&
        current_module_->frontend != nullptr) {
        if (const auto *binding = session::lookupBinding(
                *current_resolution_, expr.text, expr.scope, current_module_->frontend->scopes())) {
            if (binding->local) {
                const auto slot = localSlot(binding->local);
                return emitSlotLoad(slot, typeOfLocal(binding->local));
            }
        }
    }

    hir::HirVar var;
    var.name    = interner_.intern(expr.text);
    var.version = 0;
    if (const auto *resolved = findResolvedExpr(expr.id);
        resolved != nullptr && resolved->foreignFunction != nullptr)
        var.name = interner_.intern(resolved->foreignFunction->linkageName);
    return addExpr(std::move(var));
}

hir::HirExprId HirLowerModern::lowerLValueAddr(frontend::ExprId id) {
    if (!id || current_module_ == nullptr || current_module_->frontend == nullptr ||
        id.value > current_module_->frontend->expressions().size())
        return hir::kInvalidHirExpr;
    const auto &expr = current_module_->frontend->expressions()[id.value - 1U];
    if (expr.kind == frontend::ExprKind::Name) {
        if (const auto *resolved = findResolvedExpr(id); resolved != nullptr && resolved->local) {
            const auto slot     = localSlot(resolved->local);
            const auto local_ty = typeOfLocal(resolved->local);
            for (auto it = narrowing_stack_.rbegin(); it != narrowing_stack_.rend(); ++it) {
                if (it->local == resolved->local && it->optionalPayload) {
                    const auto payload_field = addExpr(hir::HirField{
                        addExpr(hir::HirSlotAddr{slot, local_ty}), 0U, it->type, local_ty});
                    return addExpr(hir::HirUnary{hir::HirUnaryOp::Ref, payload_field,
                                                 types_.internPtr(it->type)});
                }
                if (it->local == resolved->local && it->opaquePayload) {
                    // A narrowed opaque value is still stored as `{ *void,
                    // typeId }`. Spill the unchecked payload extraction into a
                    // temporary so its address has the narrowed concrete type.
                    const auto temp_slot = next_slot_++;
                    current_fn_->blocks[current_block_].insts.push(
                        emitSlotAlloca(temp_slot, it->type));
                    hir::HirOpaqueCast cast;
                    cast.value       = emitSlotLoad(slot, local_ty);
                    cast.from        = local_ty;
                    cast.to          = it->type;
                    cast.opaque_type = local_ty;
                    cast.result_type = it->type;
                    cast.type_id     = stableConcreteTypeId(it->type);
                    cast.checked     = false;
                    current_fn_->blocks[current_block_].insts.push(
                        emitSlotStore(temp_slot, addExpr(std::move(cast))));
                    return addExpr(hir::HirSlotAddr{temp_slot, it->type});
                }
            }
            return addExpr(hir::HirSlotAddr{slot, local_ty});
        }
        return hir::kInvalidHirExpr;
    }
    if (expr.kind == frontend::ExprKind::Field || expr.kind == frontend::ExprKind::Index) {
        if (current_types_ != nullptr && expr.kind == frontend::ExprKind::Field) {
            if (const auto *base = current_types_->traitQualifiedReceiverBase.get(expr.id.value))
                return lowerLValueAddr(frontend::ExprId{*base});
        }
        const auto value = lowerExpr(id);
        if (value == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        hir::HirUnary unary;
        unary.op      = hir::HirUnaryOp::Ref;
        unary.operand = value;
        unary.type    = types_.internPtr(typeOfExpr(id));
        return addExpr(std::move(unary));
    }
    if (expr.kind == frontend::ExprKind::Unary && expr.text == "*" && !expr.operands.empty())
        return lowerExpr(expr.operands[0]);
    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerUnary(const frontend::Expression &expr,
                                          const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    if (expr.text == "must")
        return lowerMust(expr, type);
    if (expr.text == "raw") {
        const auto operand_type = typeOfExpr(expr.operands[0]);
        if (types_.kindOf(operand_type) == types::TypeKind::Optional)
            return lowerRawOptional(expr, type);
        return lowerExpr(expr.operands[0]);
    }
    const auto operand = lowerExpr(expr.operands[0]);
    if (operand == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    hir::HirUnary unary;
    unary.op      = sema::mapUnaryOp(expr.text);
    unary.operand = operand;
    unary.type    = type;
    return addExpr(std::move(unary));
}

hir::HirExprId HirLowerModern::lowerBinary(const frontend::Expression &expr,
                                           const types::TypeId type) {
    if (expr.operands.size() != 2U)
        return hir::kInvalidHirExpr;
    const auto lhs = lowerExpr(expr.operands[0]);
    const auto rhs = lowerExpr(expr.operands[1]);
    if (lhs == hir::kInvalidHirExpr || rhs == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    hir::HirBinary binary;
    binary.lhs          = lhs;
    binary.rhs          = rhs;
    binary.op           = sema::mapBinaryOp(expr.text);
    binary.type         = type;
    binary.operand_type = typeOfExpr(expr.operands[0]);
    return addExpr(std::move(binary));
}

hir::HirExprId HirLowerModern::lowerCall(const frontend::Expression &expr) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;

    const frontend::ExprId callee_id = expr.operands[0];
    const auto &callee_expr = current_module_->frontend->expressions()[callee_id.value - 1U];

    // A Field/Arrow callee is only a method call when a matching method
    // declaration actually exists on the receiver's struct type. Module
    // aliases and callable fields also parse as Field/Arrow, so resolve
    // first and fall back to a plain call when nothing matches.
    const frontend::Declaration *method_decl      = nullptr;
    const session::ModuleArtifact *owner_artifact = nullptr;
    if ((callee_expr.kind == frontend::ExprKind::Field ||
         callee_expr.kind == frontend::ExprKind::Arrow) &&
        !callee_expr.operands.empty()) {
        const sema::modern::TypeId base_type =
            sema_.typeTable().stripQualifiers(semaTypeOfExpr(callee_expr.operands[0]));
        sema::modern::TypeId pointee = base_type;
        if (base_type && sema_.typeTable().kindOf(base_type) == TypeKind::Pointer) {
            if (const auto *ptr = sema_.typeTable().pointer(base_type))
                pointee = sema_.typeTable().stripQualifiers(ptr->pointee);
        } else if (base_type && sema_.typeTable().kindOf(base_type) == TypeKind::Optional) {
            if (const auto *opt = sema_.typeTable().optional(base_type))
                pointee = sema_.typeTable().stripQualifiers(opt->inner);
        }
        const auto *st = sema_.typeTable().struct_type(pointee);
        if (st != nullptr) {
            std::string owner_name(st->name);
            if (const size_t angle = owner_name.find('<'); angle != std::string::npos)
                owner_name.resize(angle);
            // Search the calling module first (same behavior as sema), then
            // fall back to the declaring module when the owner was imported.
            const auto findIn = [&](const session::ModuleArtifact &module,
                                    const frontend::Declaration **out) {
                for (const auto &decl : module.frontend->declarations()) {
                    if (decl.kind != frontend::DeclKind::Function)
                        continue;
                    if (decl.ownerName != owner_name || decl.name != callee_expr.text)
                        continue;
                    *out = &decl;
                    return true;
                }
                return false;
            };
            const frontend::Declaration *candidate = nullptr;
            if (findIn(*current_module_, &candidate)) {
                method_decl    = candidate;
                owner_artifact = current_module_;
            } else {
                for (const auto &module_ptr : snapshot_.modules()) {
                    const auto &module = *module_ptr;
                    if (module.key == current_module_->key || module.frontend == nullptr)
                        continue;
                    if (findIn(module, &candidate)) {
                        method_decl    = candidate;
                        owner_artifact = &module;
                        break;
                    }
                }
            }
        }
    }

    // Sema records the exact overload target across module boundaries. Prefer
    // it so overload selection and generic instantiation use the same decl
    // (and module) that type-checked the call. This is also the only path that
    // accepts trait methods, which sema has already filtered by conformance.
    if (const auto *decl = methodDeclFromTarget(callee_id, &owner_artifact); decl != nullptr) {
        method_decl = decl;
        if (!method_decl->body && callee_expr.kind == frontend::ExprKind::Field &&
            !callee_expr.operands.empty()) {
            // Interface requirements are declaration-only. After monomorphization
            // of the generic body, resolve the call to the concrete struct method
            // that satisfies the structural interface bound.
            const sema::modern::TypeId base_type =
                sema_.typeTable().stripQualifiers(semaTypeOfExpr(callee_expr.operands[0]));
            sema::modern::TypeId pointee = base_type;
            if (base_type && sema_.typeTable().kindOf(base_type) == TypeKind::Pointer) {
                if (const auto *ptr = sema_.typeTable().pointer(base_type))
                    pointee = sema_.typeTable().stripQualifiers(ptr->pointee);
            } else if (base_type && sema_.typeTable().kindOf(base_type) == TypeKind::Optional) {
                if (const auto *opt = sema_.typeTable().optional(base_type))
                    pointee = sema_.typeTable().stripQualifiers(opt->inner);
            }
            const auto *st = sema_.typeTable().struct_type(pointee);
            if (st != nullptr) {
                std::string owner_name(st->name);
                if (const size_t angle = owner_name.find('<'); angle != std::string::npos)
                    owner_name.resize(angle);
                const auto findConcrete = [&](const session::ModuleArtifact &module,
                                              const frontend::Declaration **out) {
                    for (const auto &candidate : module.frontend->declarations()) {
                        if (candidate.kind != frontend::DeclKind::Function ||
                            candidate.ownerName != owner_name || candidate.name != callee_expr.text)
                            continue;
                        *out = &candidate;
                        return true;
                    }
                    return false;
                };
                const frontend::Declaration *concrete = nullptr;
                if (findConcrete(*current_module_, &concrete)) {
                    method_decl    = concrete;
                    owner_artifact = current_module_;
                } else {
                    for (const auto &module_ptr : snapshot_.modules()) {
                        const auto &module = *module_ptr;
                        if (module.key == current_module_->key || module.frontend == nullptr)
                            continue;
                        if (findConcrete(module, &concrete)) {
                            method_decl    = concrete;
                            owner_artifact = &module;
                            break;
                        }
                    }
                }
            }
        }
    }

    // `p.method()` on a `dyn`/fat-pointer receiver dispatches through the
    // vtable embedded in the receiver. Sema resolved the call to the trait
    // requirement, which also tells us the slot index in trait declaration
    // order.
    if (method_decl != nullptr &&
        (callee_expr.kind == frontend::ExprKind::Field ||
         callee_expr.kind == frontend::ExprKind::Arrow) &&
        !callee_expr.operands.empty()) {
        const sema::modern::TypeId dyn_base =
            sema_.typeTable().stripQualifiers(semaTypeOfExpr(callee_expr.operands[0]));
        if (sema_.typeTable().kindOf(dyn_base) == sema::modern::TypeKind::Dyn) {
            const auto *dyn_ty                  = sema_.typeTable().dyn_type(dyn_base);
            const sema::modern::TypeId trait_ty = sema_.typeTable().canonical(
                dyn_ty != nullptr ? dyn_ty->target : sema::modern::kInvalidTypeId);
            const auto *trait = sema_.typeTable().trait(trait_ty);
            if (trait == nullptr)
                return hir::kInvalidHirExpr;

            uint32_t slot_index = ~0U;
            uint32_t cursor     = 0;
            const auto findSlot = [&](const session::ModuleArtifact &module) {
                for (const auto &candidate : module.frontend->declarations()) {
                    if (candidate.kind != frontend::DeclKind::Function ||
                        candidate.ownerName != trait->name || candidate.name.empty() ||
                        candidate.name == "self")
                        continue;
                    if (candidate.name == callee_expr.text)
                        slot_index = cursor;
                    ++cursor;
                }
            };
            findSlot(*current_module_);
            for (const auto &module_ptr : snapshot_.modules()) {
                if (module_ptr->key == current_module_->key || module_ptr->frontend == nullptr)
                    continue;
                findSlot(*module_ptr);
            }
            if (slot_index == ~0U)
                return hir::kInvalidHirExpr;

            const auto *method_sema = owner_artifact != nullptr
                                          ? sema_.findModuleSema(owner_artifact->key)
                                          : sema_.findModuleSema(current_module_->key);
            const auto *fn =
                method_sema != nullptr
                    ? sema_.typeTable().function(method_sema->typeOfDecl(method_decl->id))
                    : nullptr;
            if (fn == nullptr)
                return hir::kInvalidHirExpr;

            const bool has_receiver =
                !method_decl->parameters.empty() && method_decl->parameters.front().name == "self";
            memory::DynArray<types::TypeId> lowered_params(arena_);
            for (size_t pi = has_receiver ? 1U : 0U; pi < fn->params.size(); ++pi)
                lowered_params.push(lowerType(fn->params[pi]));
            const types::TypeId lowered_fn = types_.internFn(lowered_params, lowerType(fn->result));

            hir::HirDynCall dyncall(arena_);
            dyncall.receiver     = lowerExpr(callee_expr.operands[0]);
            dyncall.vtable_name  = interner_.intern(trait->name);
            dyncall.slot_index   = slot_index;
            dyncall.has_receiver = has_receiver;
            dyncall.result_type  = lowerType(fn->result);
            dyncall.fn_type      = lowered_fn;

            const size_t dyn_slice_param =
                !method_decl->parameters.empty() && method_decl->parameters.back().isVariadicSlice
                    ? fn->params.size() - 1U
                    : fn->params.size();
            const bool dyn_explicit_slice =
                dyn_slice_param < fn->params.size() &&
                expr.operands.size() == dyn_slice_param + 1U && !expr.operands.empty() &&
                (types_.kindOf(typeOfExpr(expr.operands.back())) == types::TypeKind::Slice ||
                 types_.kindOf(typeOfExpr(expr.operands.back())) == types::TypeKind::Array);
            const bool dyn_collect_tail =
                dyn_slice_param < fn->params.size() && !dyn_explicit_slice;
            bool dyn_tail_lowered = false;
            for (size_t index = 1; index < expr.operands.size(); ++index) {
                const size_t call_index = has_receiver ? index : index - 1U;
                if (dyn_collect_tail && call_index >= dyn_slice_param) {
                    std::vector<frontend::ExprId> tail;
                    tail.reserve(expr.operands.size() - index);
                    for (size_t tail_index = index; tail_index < expr.operands.size(); ++tail_index)
                        tail.push_back(expr.operands[tail_index]);
                    auto slice = lowerVariadicSliceTail(fn->params[dyn_slice_param], tail);
                    if (slice == hir::kInvalidHirExpr)
                        return hir::kInvalidHirExpr;
                    dyncall.args.push(slice);
                    dyncall.arg_types.push(lowerType(fn->params[dyn_slice_param]));
                    dyn_tail_lowered = true;
                    break;
                }
                if (call_index >= fn->params.size())
                    continue;
                auto argument = lowerExpr(expr.operands[index]);
                if (argument == hir::kInvalidHirExpr)
                    return hir::kInvalidHirExpr;
                argument = lowerCoerceToTarget(lowerType(fn->params[call_index]),
                                               expr.operands[index], argument);
                const sema::modern::TypeId param_sema =
                    sema_.typeTable().kindOf(fn->params[call_index]) == sema::modern::TypeKind::Dyn
                        ? fn->params[call_index]
                        : sema_.typeTable().canonical(fn->params[call_index]);
                if (sema_.typeTable().kindOf(param_sema) == sema::modern::TypeKind::Dyn) {
                    dyncall.args.push(argument);
                    dyncall.arg_types.push(lowerType(fn->params[call_index]));
                    argument = lowerCoerceToDyn(fn->params[call_index], expr.operands[index],
                                                argument, fn->params[call_index]);
                    dyncall.args.back() = argument;
                } else {
                    dyncall.args.push(argument);
                    dyncall.arg_types.push(lowerType(fn->params[call_index]));
                    argument =
                        lowerCoerceToOpaque(fn->params[call_index], expr.operands[index], argument);
                    if (types_.kindOf(lowerType(fn->params[call_index])) ==
                            types::TypeKind::Optional &&
                        types_.kindOf(lowerType(param_sema)) != types::TypeKind::Optional)
                        argument =
                            lowerCoerceToOptional(lowerType(fn->params[call_index]), argument);
                    dyncall.args.back() = argument;
                }
            }
            if (method_decl != nullptr) {
                const session::ModuleArtifact *dyn_decl_module =
                    owner_artifact != nullptr ? owner_artifact : current_module_;
                const comptime::InstantiationInstance *dyn_decl_instance = nullptr;
                if (const auto *instantiations = sema_.instantiations();
                    instantiations != nullptr) {
                    if (const auto *binding =
                            instantiations->callBinding(current_module_->key, callee_id))
                        dyn_decl_instance = instantiations->at(binding->instance);
                }
                const size_t dyn_explicit_fixed =
                    has_receiver ? dyncall.args.size() - 1U : dyncall.args.size();
                for (size_t index = (has_receiver ? 1U : 0U) + dyn_explicit_fixed;
                     index < method_decl->parameters.size() && index < dyn_slice_param; ++index) {
                    if (!method_decl->parameters[index].defaultValue)
                        continue;
                    auto value = lowerDefaultWithTarget(*dyn_decl_module, dyn_decl_instance,
                                                        method_decl->parameters[index].defaultValue,
                                                        fn->params[index]);
                    if (value == hir::kInvalidHirExpr)
                        return hir::kInvalidHirExpr;
                    value = lowerCoerceToOpaque(fn->params[index], {}, value);
                    dyncall.args.push(lowerCoerceToDyn(fn->params[index],
                                                       method_decl->parameters[index].defaultValue,
                                                       value, fn->params[index]));
                    dyncall.arg_types.push(lowerType(fn->params[index]));
                }
            }
            if (dyn_collect_tail && !dyn_tail_lowered) {
                auto slice = lowerVariadicSliceTail(fn->params[dyn_slice_param], {});
                if (slice == hir::kInvalidHirExpr)
                    return hir::kInvalidHirExpr;
                slice = lowerCoerceToOpaque(fn->params[dyn_slice_param], {}, slice);
                dyncall.args.push(slice);
                dyncall.arg_types.push(lowerType(fn->params[dyn_slice_param]));
            }
            return addExpr(std::move(dyncall));
        }
    }

    // A method with a `self` receiver receives an implicit owner pointer
    // argument. Methods without self are static in this compiler.
    const bool is_receiver_method = method_decl != nullptr && !method_decl->parameters.empty() &&
                                    method_decl->parameters.front().name == "self";
    const bool is_method_call = is_receiver_method;

    hir::HirExprId callee = hir::kInvalidHirExpr;
    if (!is_method_call) {
        if (method_decl == nullptr) {
            callee = lowerExpr(callee_id);
            if (callee == hir::kInvalidHirExpr)
                return hir::kInvalidHirExpr;
        } // Static methods keep callee = kInvalid and resolve by symbol below.
    }

    memory::DynArray<hir::HirExprId> args(arena_);
    memory::DynArray<types::TypeId> arg_types(arena_);

    if (is_receiver_method) {
        frontend::ExprId base_id = callee_expr.operands[0];
        if (current_types_ != nullptr) {
            if (const auto *base = current_types_->traitQualifiedReceiverBase.get(base_id.value))
                base_id = frontend::ExprId{*base};
        }
        // `->` passes a pointer value; `.` passes the address of a value
        // receiver. Pointer-valued bases passed through `.method()` also pass
        // the pointer itself, matching sema's method-call argument type.
        hir::HirExprId self_arg = hir::kInvalidHirExpr;
        const auto base_sema_ty = semaTypeOfExpr(base_id);
        const auto base_hir_ty  = typeOfExpr(base_id);
        const sema::modern::TypeId base_sema_resolved =
            sema_.typeTable().stripQualifiers(base_sema_ty);
        const auto *base_optional = sema_.typeTable().optional(base_sema_resolved);
        const bool base_optional_aggregate =
            base_optional != nullptr &&
            sema_.typeTable().kindOf(sema_.typeTable().stripQualifiers(base_optional->inner)) !=
                TypeKind::Pointer;
        const bool optional_has_exact_owner =
            base_optional != nullptr && method_decl != nullptr &&
            sema_.typeTable().typeToString(base_sema_resolved) == method_decl->ownerName;
        const bool base_is_ptr = base_hir_ty != types::kErrorType &&
                                 base_hir_ty != types::kInvalidType &&
                                 (types_.kindOf(base_hir_ty) == types::TypeKind::Ptr ||
                                  types_.kindOf(base_hir_ty) == types::TypeKind::Optional);
        if (base_optional_aggregate && optional_has_exact_owner)
            self_arg = lowerLValueAddr(base_id);
        else if (base_optional_aggregate) {
            // The receiver points at the stored payload, never at the
            // aggregate `{payload, tag}` wrapper.
            const auto slot = next_slot_++;
            current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, base_hir_ty));
            const auto base_value = lowerExpr(base_id);
            current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, base_value));
            const auto payload_type =
                lowerType(sema_.typeTable().stripQualifiers(base_optional->inner));
            const auto payload_field = addExpr(hir::HirField{
                addExpr(hir::HirSlotAddr{slot, base_hir_ty}), 0U, payload_type, base_hir_ty});
            self_arg                 = addExpr(
                hir::HirUnary{hir::HirUnaryOp::Ref, payload_field, types_.internPtr(payload_type)});
        } else if (callee_expr.kind == frontend::ExprKind::Arrow || base_is_ptr)
            self_arg = lowerExpr(base_id);
        else
            self_arg = lowerLValueAddr(base_id);
        if (self_arg == hir::kInvalidHirExpr) {
            const auto base_hir_type = typeOfExpr(base_id);
            const auto value         = lowerExpr(base_id);
            const auto self_slot     = next_slot_++;
            current_fn_->blocks[current_block_].insts.push(
                emitSlotAlloca(self_slot, base_hir_type));
            current_fn_->blocks[current_block_].insts.push(emitSlotStore(self_slot, value));
            self_arg = addExpr(hir::HirSlotAddr{self_slot, base_hir_type});
        }
        args.push(self_arg);
        const sema::modern::TypeId callee_sema_type0 = semaTypeOfExpr(callee_id);
        const auto *callee_fn0 = callee_sema_type0 != sema::modern::kInvalidTypeId
                                     ? sema_.typeTable().function(callee_sema_type0)
                                     : nullptr;
        const auto self_type   = callee_fn0 != nullptr && !callee_fn0->params.empty()
                                     ? lowerType(callee_fn0->params[0])
                                     : (base_sema_ty ? lowerType(base_sema_ty) : types::kInvalidType);
        if (self_type != types::kInvalidType)
            arg_types.push(self_type);
    }

    // Resolve the callee signature once so variadic-slice auto-collection can
    // be computed from the same declaration used by sema/overload selection.
    sema::modern::TypeId callee_sema_type = semaTypeOfExpr(callee_id);
    if (callee_sema_type == sema::modern::kInvalidTypeId) {
        if (const auto *target = overloadTarget(callee_id)) {
            const auto *target_sema = sema_.findModuleSema(target->module);
            if (target_sema != nullptr)
                callee_sema_type = target_sema->typeOfDecl(target->decl);
        } else if (const auto *resolved = findResolvedExpr(callee_id)) {
            const session::ModuleArtifact *decl_module = nullptr;
            const auto *decl = resolvedFunctionDecl(*resolved, &decl_module);
            if (decl != nullptr && decl_module != nullptr) {
                if (const auto *decl_sema = sema_.findModuleSema(decl_module->key))
                    callee_sema_type = decl_sema->typeOfDecl(decl->id);
            }
        }
    }
    const auto *callee_fn = callee_sema_type != sema::modern::kInvalidTypeId
                                ? sema_.typeTable().function(callee_sema_type)
                                : nullptr;
    size_t slice_param    = ~static_cast<size_t>(0);
    if (callee_fn != nullptr && method_decl != nullptr && !method_decl->parameters.empty() &&
        method_decl->parameters.back().isVariadicSlice) {
        slice_param = callee_fn->params.size() - 1U;
    } else if (const auto *resolved = findResolvedExpr(callee_id);
               resolved != nullptr && resolved->isVariadicSlice && callee_fn != nullptr &&
               !callee_fn->params.empty()) {
        slice_param = callee_fn->params.size() - 1U;
    } else if (const auto *target = overloadTarget(callee_id);
               target != nullptr && callee_fn != nullptr && !callee_fn->params.empty()) {
        const auto *target_artifact = snapshot_.findModule(target->module);
        if (target_artifact != nullptr && target_artifact->frontend != nullptr &&
            target->decl.value <= target_artifact->frontend->declarations().size()) {
            const auto &target_decl =
                target_artifact->frontend->declarations()[target->decl.value - 1U];
            if (!target_decl.parameters.empty() && target_decl.parameters.back().isVariadicSlice)
                slice_param = callee_fn->params.size() - 1U;
        }
    }

    // A single trailing slice is an explicit `[]T` argument, not one element
    // to auto-collect. The frontend marks it with the same type as the slice
    // parameter, so the check mirrors sema's `explicit_slice_arg` decision.
    const bool explicit_slice_arg =
        slice_param != ~static_cast<size_t>(0) && expr.operands.size() > 1 && [&]() {
            const size_t last_index = expr.operands.size() - 1U;
            const size_t last_call  = is_receiver_method ? last_index : last_index - 1U;
            if (last_call != slice_param)
                return false;
            const auto last_type = typeOfExpr(expr.operands.back());
            return types_.kindOf(last_type) == types::TypeKind::Slice ||
                   types_.kindOf(last_type) == types::TypeKind::Array;
        }();
    const bool collect_tail = slice_param != ~static_cast<size_t>(0) && !explicit_slice_arg;
    bool tail_lowered       = false;

    for (size_t index = 1; index < expr.operands.size(); ++index) {
        const size_t call_index          = is_receiver_method ? index : index - 1U;
        const bool is_first_tail_element = collect_tail && call_index >= slice_param;
        if (is_first_tail_element) {
            std::vector<frontend::ExprId> tail;
            tail.reserve(expr.operands.size() - index);
            for (size_t tail_index = index; tail_index < expr.operands.size(); ++tail_index)
                tail.push_back(expr.operands[tail_index]);
            const sema::modern::TypeId slice_sema = callee_fn->params[slice_param];
            auto slice                            = lowerVariadicSliceTail(slice_sema, tail);
            if (slice == hir::kInvalidHirExpr)
                return hir::kInvalidHirExpr;
            if (sema_.typeTable().kindOf(slice_sema) == sema::modern::TypeKind::Dyn ||
                sema_.typeTable().kindOf(sema_.typeTable().canonical(slice_sema)) ==
                    sema::modern::TypeKind::Dyn)
                slice = lowerCoerceToDyn(slice_sema, expr.operands[index], slice, slice_sema);
            else
                slice = lowerCoerceToOpaque(slice_sema, expr.operands[index], slice);
            args.push(slice);
            arg_types.push(lowerType(slice_sema));
            tail_lowered = true;
            break;
        }
        const auto &arg_expr =
            current_module_->frontend->expressions()[expr.operands[index].value - 1U];
        const bool annotated =
            arg_expr.kind == frontend::ExprKind::OwnershipCoerce && !arg_expr.operands.empty();
        const frontend::ExprId inner_id = annotated ? arg_expr.operands[0] : expr.operands[index];
        auto argument                   = lowerExpr(inner_id);
        if (annotated) {
            auto address = lowerLValueAddr(inner_id);
            if (address == hir::kInvalidHirExpr) {
                const auto inner_type = typeOfExpr(inner_id);
                const auto slot       = next_slot_++;
                current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, inner_type));
                current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, argument));
                address = addExpr(hir::HirSlotAddr{slot, inner_type});
            }
            argument = address;
        }
        if (argument == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        const auto argument_type = typeOfExpr(expr.operands[index]);
        if (argument_type != types::kInvalidType)
            arg_types.push(argument_type);
        const auto param_sema_raw = callee_fn != nullptr && call_index < callee_fn->params.size()
                                        ? callee_fn->params[call_index]
                                        : sema::modern::kInvalidTypeId;
        const auto param_type     = param_sema_raw != sema::modern::kInvalidTypeId
                                        ? lowerType(param_sema_raw)
                                        : types::kInvalidType;
        hir::HirExprId lowered_argument =
            lowerCoerceToTarget(param_type, expr.operands[index], argument);
        if (param_sema_raw != sema::modern::kInvalidTypeId) {
            const auto param_sema = param_sema_raw;
            if (sema_.typeTable().kindOf(param_sema) == sema::modern::TypeKind::Dyn ||
                sema_.typeTable().kindOf(sema_.typeTable().canonical(param_sema)) ==
                    sema::modern::TypeKind::Dyn)
                lowered_argument = lowerCoerceToDyn(param_sema, expr.operands[index],
                                                    lowered_argument, param_sema);
            else
                lowered_argument =
                    lowerCoerceToOpaque(param_sema, expr.operands[index], lowered_argument);
            const auto arg_sema = semaTypeOfExpr(expr.operands[index]);
            if (types_.kindOf(param_type) == types::TypeKind::Optional)
                lowered_argument =
                    lowerCoerceToOptionalDepth(param_type, param_sema, arg_sema, lowered_argument);
        }
        args.push(lowered_argument);
    }
    // Default arguments are validated by sema but not present in the caller's
    // expression nodes. Materialize missing fixed parameters as ordinary
    // arguments once the explicit (or explicit-slice) tail is known.
    const session::ModuleArtifact *decl_module       = owner_artifact;
    const comptime::InstantiationInstance *decl_inst = nullptr;
    const frontend::Declaration *param_decl          = method_decl;
    const size_t receiver_offset                     = is_receiver_method ? 1U : 0U;
    if (param_decl == nullptr) {
        if (const auto *target = overloadTarget(callee_id); target != nullptr) {
            decl_module = snapshot_.findModule(target->module);
            param_decl  = decl_module != nullptr ? findDecl(*decl_module, target->decl) : nullptr;
        } else if (const auto *resolved = findResolvedExpr(callee_id)) {
            param_decl = resolvedFunctionDecl(*resolved, &decl_module);
        }
    }
    if (decl_module == nullptr)
        decl_module = current_module_;
    if (const auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
        if (const auto *binding = instantiations->callBinding(current_module_->key, callee_id))
            decl_inst = instantiations->at(binding->instance);
    }
    if (param_decl != nullptr && decl_module != nullptr) {
        const size_t explicit_fixed = args.size() - receiver_offset;
        const size_t max_fixed =
            slice_param != ~static_cast<size_t>(0) ? slice_param : callee_fn->params.size();
        for (size_t index = receiver_offset + explicit_fixed;
             index < param_decl->parameters.size() && index < max_fixed; ++index) {
            if (!param_decl->parameters[index].defaultValue)
                continue;
            auto value = lowerDefaultWithTarget(*decl_module, decl_inst,
                                                param_decl->parameters[index].defaultValue,
                                                callee_fn->params[index]);
            if (value == hir::kInvalidHirExpr)
                return hir::kInvalidHirExpr;
            args.push(value);
            arg_types.push(lowerType(callee_fn->params[index]));
        }
    }
    if (collect_tail && !tail_lowered) {
        // A variadic-slice callee is still callable with an empty tail. Lower
        // `[0, 0]` so the call keeps the slice parameter in its signature.
        const sema::modern::TypeId slice_sema = callee_fn->params[slice_param];
        auto slice                            = lowerVariadicSliceTail(slice_sema, {});
        if (slice == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        args.push(slice);
        arg_types.push(lowerType(slice_sema));
    }

    hir::HirCall call{callee, std::move(args), std::move(arg_types)};
    call.resolved_fn = symbols::kInvalidSym;
    if (callee != hir::kInvalidHirExpr) {
        const auto callee_fn_type = typeOfExpr(callee_id);
        if (callee_fn_type != types::kInvalidType &&
            types_.kindOf(callee_fn_type) == types::TypeKind::Fn)
            call.fn_type = callee_fn_type;
    }
    if (method_decl != nullptr) {
        const auto *method_module_artifact =
            owner_artifact != nullptr ? owner_artifact : current_module_;
        const auto key = internFunctionKey(interner_, method_module_artifact->key, method_decl->id);
        if (const auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
            if (const auto *binding =
                    instantiations->callBinding(current_module_->key, callee_id)) {
                const auto *instance = instantiations->at(binding->instance);
                for (const auto &function : functions_) {
                    if (function.instance != nullptr && function.module != nullptr &&
                        function.module->key == binding->module &&
                        function.instance->decl == method_decl->id && instance != nullptr &&
                        function.instance->args == instance->args) {
                        call.resolved_fn = function.sym_id;
                        break;
                    }
                }
            }
        }
        if (call.resolved_fn == symbols::kInvalidSym) {
            if (const auto *function_index = function_index_by_key_.get(key))
                call.resolved_fn = functions_[*function_index].sym_id;
        }
    } else {
        const auto *target = overloadTarget(callee_id);
        // Sema already picked one declaration out of an overload set; re-resolving
        // here would silently fall back to the first candidate.
        if (target != nullptr) {
            const auto key = internFunctionKey(interner_, target->module, target->decl);
            if (auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
                if (const auto *binding =
                        instantiations->callBinding(current_module_->key, callee_id)) {
                    const auto *instance = instantiations->at(binding->instance);
                    for (const auto &function : functions_) {
                        if (function.instance != nullptr && function.module != nullptr &&
                            function.module->key == binding->module &&
                            function.instance->decl == target->decl && instance != nullptr &&
                            function.instance->args == instance->args) {
                            call.resolved_fn = function.sym_id;
                            break;
                        }
                    }
                }
            }
            if (call.resolved_fn == symbols::kInvalidSym) {
                if (const auto *function_index = function_index_by_key_.get(key))
                    call.resolved_fn = functions_[*function_index].sym_id;
            }
        } else if (const auto *resolved = findResolvedExpr(callee_id)) {
            if (auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
                if (const auto *binding =
                        instantiations->callBinding(current_module_->key, callee_id)) {
                    const session::ModuleArtifact *resolved_module = nullptr;
                    const auto *decl     = resolvedFunctionDecl(*resolved, &resolved_module);
                    const auto *instance = instantiations->at(binding->instance);
                    if (decl != nullptr && resolved_module != nullptr && instance != nullptr) {
                        for (const auto &function : functions_) {
                            if (function.instance != nullptr && function.module != nullptr &&
                                function.module->key == resolved_module->key &&
                                function.instance->decl == decl->id &&
                                function.instance->args == instance->args) {
                                call.resolved_fn = function.sym_id;
                                break;
                            }
                        }
                    }
                }
            }
            if (call.resolved_fn == symbols::kInvalidSym)
                call.resolved_fn = resolvedFunctionSym(*resolved);
        }
    }
    const hir::HirExprId call_id = addExpr(std::move(call));
    if (call_id != hir::kInvalidHirExpr) {
        const session::ModuleArtifact *resolved_module = nullptr;
        const auto *resolved                           = findResolvedExpr(callee_id);
        const auto *target_decl =
            resolved != nullptr ? resolvedFunctionDecl(*resolved, &resolved_module) : nullptr;
        const auto sema_state_callee =
            semaTypeOfExpr(callee_id) != sema::modern::kInvalidTypeId &&
            sema_.typeTable().kindOf(semaTypeOfExpr(callee_id)) == sema::modern::TypeKind::State;
        if ((target_decl != nullptr && target_decl->kind == frontend::DeclKind::Function &&
             target_decl->functionKind == frontend::FunctionKind::State) ||
            sema_state_callee) {
            auto &hir_call      = std::get<hir::HirCall>(hir_.getExprMut(call_id));
            hir_call.usesTailCC = true;
            if (current_fn_ != nullptr)
                current_fn_->usesTailCC = true;
        }
    }
    if (nra_ != nullptr) {
        const auto *call_fact = nra_->callFact(expr.id);
        if (call_fact != nullptr && call_fact->hasResidual()) {
            auto &attrs = hir_.attrs().call(call_id);
            if (call_fact->returnsArgument != ~0U)
                attrs.returnsArg = call_fact->returnsArgument;
            for (const auto escape : call_fact->argEscapes)
                attrs.args.emplace(hir::HirCallArgAttr{mapHirEscape(escape)});
            if (call_fact->duplicatedShareOrView) {
                while (attrs.args.size() < call_fact->argEscapes.size())
                    attrs.args.emplace(hir::HirCallArgAttr{hir::HirCallEscape::None});
            }
        }
    }
    return call_id;
}

hir::HirExprId HirLowerModern::lowerBlock(const frontend::Expression &expr) {
    cleanup_stack_.push_back(CleanupFrame(arena_));
    pending_defers_.emplace_back();
    hir::HirExprId last = hir::kInvalidHirExpr;
    for (const auto statement : expr.statements) {
        if (!pending_defers_.back().empty() && statement) {
            const auto &stmt = current_module_->frontend->statements()[statement.value - 1U];
            if (stmt.kind == frontend::StmtKind::Return || stmt.kind == frontend::StmtKind::Break ||
                stmt.kind == frontend::StmtKind::Continue ||
                stmt.kind == frontend::StmtKind::Jump) {
                if (!flushPendingDefers())
                    return hir::kInvalidHirExpr;
            }
        }
        if (current_fn_->blocks[current_block_].terminator != hir::kInvalidHirExpr)
            break;
        if (!lowerStatement(statement, last))
            return hir::kInvalidHirExpr;
    }
    if (!flushPendingDefers())
        return hir::kInvalidHirExpr;
    pending_defers_.pop_back();

    auto frame = std::move(cleanup_stack_.back());
    cleanup_stack_.pop_back();
    if (!frame.exprs.empty() &&
        current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr) {
        hir::HirCleanup cleanup(arena_);
        cleanup.exprs = std::move(frame.exprs);
        current_fn_->blocks[current_block_].insts.push(addExpr(std::move(cleanup)));
    }
    return last;
}

bool HirLowerModern::flushPendingDefers() {
    if (pending_defers_.empty())
        return true;
    for (const auto id : pending_defers_.back()) {
        if (current_fn_->blocks[current_block_].terminator != hir::kInvalidHirExpr)
            break;
        if (!lowerDeferBody(id))
            return false;
    }
    return true;
}

bool HirLowerModern::lowerDeferBody(frontend::StmtId id) {
    if (!id || current_module_ == nullptr ||
        id.value > current_module_->frontend->statements().size())
        return true;
    const auto &statement = current_module_->frontend->statements()[id.value - 1U];
    if (!statement.expression) {
        diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                      "defer requires an expression or block", {});
        return false;
    }
    if (const auto *body =
            current_module_->frontend->expressions()[statement.expression.value - 1U].kind ==
                    frontend::ExprKind::Block
                ? &current_module_->frontend->expressions()[statement.expression.value - 1U]
                : nullptr;
        body != nullptr) {
        const auto body_id = lowerDeferBlock(*body);
        if (body_id == hir::kInvalidHirExpr) {
            // An empty block still produces a cleanup expression.
            return false;
        }
        if (defer_body_sink_ != nullptr)
            defer_body_sink_->push(body_id);
        else if (!cleanup_stack_.empty())
            cleanup_stack_.back().exprs.push(body_id);
        else
            current_fn_->blocks[current_block_].insts.push(body_id);
        return true;
    }
    const auto deferred = lowerExpr(statement.expression);
    if (deferred == hir::kInvalidHirExpr)
        return false;
    if (defer_body_sink_ != nullptr) {
        // A nested `defer expr;` inside `defer { ... }` runs as an ordinary
        // deferred body statement in source order.
        defer_body_sink_->push(deferred);
    } else if (!cleanup_stack_.empty()) {
        cleanup_stack_.back().exprs.push(deferred);
    } else {
        current_fn_->blocks[current_block_].insts.push(deferred);
    }
    return true;
}

void HirLowerModern::emitCleanupFrom(size_t first) {
    if (first >= cleanup_stack_.size())
        return;
    if (current_fn_->blocks[current_block_].terminator != hir::kInvalidHirExpr)
        return;

    bool any = false;
    for (size_t index = cleanup_stack_.size(); index-- > first;) {
        if (!cleanup_stack_[index].exprs.empty())
            any = true;
    }
    if (!any)
        return;

    hir::HirCleanup cleanup(arena_);
    for (size_t index = cleanup_stack_.size(); index-- > first;) {
        const auto &frame = cleanup_stack_[index].exprs;
        for (size_t inner = frame.size(); inner > 0U; --inner)
            cleanup.exprs.push(frame[inner - 1U]);
    }
    current_fn_->blocks[current_block_].insts.push(addExpr(std::move(cleanup)));
}

hir::HirExprId HirLowerModern::lowerDeferBlock(const frontend::Expression &expr) {
    auto *saved_sink = defer_body_sink_;
    memory::DynArray<hir::HirExprId> sink(arena_);
    defer_body_sink_    = &sink;
    hir::HirExprId last = hir::kInvalidHirExpr;
    for (const auto statement : expr.statements) {
        if (current_fn_->blocks[current_block_].terminator != hir::kInvalidHirExpr)
            break;
        if (!lowerStatement(statement, last)) {
            defer_body_sink_ = saved_sink;
            return hir::kInvalidHirExpr;
        }
    }
    defer_body_sink_ = saved_sink;

    hir::HirCleanup cleanup(arena_);
    for (const auto expr_id : sink)
        cleanup.exprs.push(expr_id);
    return addExpr(std::move(cleanup));
}

hir::HirExprId HirLowerModern::lowerIf(const frontend::Expression &expr, const types::TypeId type) {
    if (expr.operands.size() < 2U)
        return hir::kInvalidHirExpr;

    const auto cond = lowerCondition(expr.operands[0]);
    if (cond == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    const bool has_else    = expr.operands.size() >= 3U;
    const bool has_value   = has_else && type != types::kVoidType && type != types::kErrorType;
    const auto result_slot = has_value ? next_slot_++ : hir::kInvalidHirSlot;
    if (has_value)
        current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(result_slot, type));

    const auto then_block  = newBlock();
    const auto else_block  = newBlock();
    const auto merge_block = newBlock();

    hir::HirBranch branch;
    branch.cond       = cond;
    branch.then_block = static_cast<hir::HirDeclId>(then_block);
    branch.else_block = static_cast<hir::HirDeclId>(has_else ? else_block : merge_block);
    setTerminator(addExpr(std::move(branch)));

    frontend::LocalId narrowed_local = {};
    types::TypeId narrowed_type      = types::kInvalidType;
    bool narrow_then                 = false;
    bool narrowed_optional_payload   = false;
    bool narrowed_opaque_payload     = false;
    const auto &condition = current_module_->frontend->expressions()[expr.operands[0].value - 1U];
    const auto makeOptionalNarrowing = [&](frontend::ExprId operand) {
        const auto *resolved = findResolvedExpr(operand);
        if (resolved == nullptr || !resolved->local)
            return;
        const sema::modern::TypeId local_sema = semaTypeOfLocal(resolved->local);
        const auto *optional =
            sema_.typeTable().optional(sema_.typeTable().stripQualifiers(local_sema));
        if (optional == nullptr || sema_.typeTable().kindOf(sema_.typeTable().stripQualifiers(
                                       optional->inner)) == TypeKind::Pointer)
            return;
        narrowed_local            = resolved->local;
        narrowed_type             = lowerType(sema_.typeTable().stripQualifiers(optional->inner));
        narrowed_optional_payload = true;
    };
    if (condition.kind == frontend::ExprKind::IsNull && !condition.operands.empty()) {
        makeOptionalNarrowing(condition.operands[0]);
    } else if (condition.kind == frontend::ExprKind::IsType && !condition.operands.empty() &&
               condition.cast_type) {
        const auto *resolved = findResolvedExpr(condition.operands[0]);
        if (resolved != nullptr && resolved->local) {
            const sema::modern::TypeId local_sema = semaTypeOfLocal(resolved->local);
            if (sema_.typeTable().kindOf(sema_.typeTable().stripQualifiers(local_sema)) ==
                TypeKind::Opaque) {
                narrowed_local          = resolved->local;
                narrowed_type           = lowerType(sema_.typeTable().lowerTypeExpr(
                    *current_module_->frontend, condition.cast_type));
                narrow_then             = true;
                narrowed_opaque_payload = true;
            }
        }
    } else if (condition.kind == frontend::ExprKind::Unary && condition.text == "not" &&
               !condition.operands.empty()) {
        const auto &inner =
            current_module_->frontend->expressions()[condition.operands[0].value - 1U];
        if (inner.kind == frontend::ExprKind::IsNull && !inner.operands.empty()) {
            makeOptionalNarrowing(inner.operands[0]);
            narrow_then = true;
        }
    }

    setCurrentBlock(then_block);
    current_fn_->blocks[then_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    cleanup_stack_.push_back(CleanupFrame(arena_));
    const size_t then_cleanup = cleanup_stack_.size() - 1U;
    if (narrowed_local && narrow_then) {
        narrowing_stack_.push_back(Narrowing{narrowed_local, narrowed_type,
                                             narrowed_optional_payload, narrowed_opaque_payload});
    }
    const auto then_value = lowerExpr(expr.operands[1]);
    if (narrowed_local && narrow_then)
        narrowing_stack_.pop_back();
    emitCleanupFrom(then_cleanup);
    cleanup_stack_.pop_back();
    if (has_value && then_value != hir::kInvalidHirExpr &&
        current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr) {
        current_fn_->blocks[current_block_].insts.push(emitSlotStore(result_slot, then_value));
    }
    if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr)
        emitJump(merge_block);

    const bool has_else_condition        = expr.operands.size() > 3U;
    const frontend::ExprId else_value_id = has_else_condition ? expr.operands[3] : expr.operands[2];
    if (has_else) {
        setCurrentBlock(else_block);
        current_fn_->blocks[else_block].insts = memory::DynArray<hir::HirExprId>(arena_);
        cleanup_stack_.push_back(CleanupFrame(arena_));
        const size_t else_cleanup = cleanup_stack_.size() - 1U;
        if (narrowed_local && !narrow_then)
            narrowing_stack_.push_back(Narrowing{
                narrowed_local, narrowed_type, narrowed_optional_payload, narrowed_opaque_payload});
        if (has_else_condition) {
            const auto else_cond = lowerCondition(expr.operands[2]);
            if (else_cond != hir::kInvalidHirExpr) {
                current_fn_->blocks[current_block_].insts.push(else_cond);
                hir::HirBranch else_branch;
                else_branch.cond       = else_cond;
                else_branch.then_block = static_cast<hir::HirDeclId>(merge_block);
                else_branch.else_block = static_cast<hir::HirDeclId>(merge_block);
                setTerminator(addExpr(std::move(else_branch)));
            }
        }
        const auto else_value = lowerExpr(else_value_id);
        if (narrowed_local && !narrow_then)
            narrowing_stack_.pop_back();
        emitCleanupFrom(else_cleanup);
        cleanup_stack_.pop_back();
        if (has_value && else_value != hir::kInvalidHirExpr &&
            current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr) {
            current_fn_->blocks[current_block_].insts.push(emitSlotStore(result_slot, else_value));
        }
        if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr)
            emitJump(merge_block);
    }

    setCurrentBlock(merge_block);
    current_fn_->blocks[merge_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    return has_value ? emitSlotLoad(result_slot, type) : hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerWhen(const frontend::Expression &expr,
                                         const types::TypeId type) {
    const size_t case_count = expr.operands.size() - 1U;
    if (case_count == 0)
        return hir::kInvalidHirExpr;

    // The subject is evaluated exactly once and spilled so every case can compare.
    const auto subject      = lowerExpr(expr.operands[0]);
    const auto subject_type = typeOfExpr(expr.operands[0]);
    if (subject == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto subject_slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(subject_slot, subject_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(subject_slot, subject));

    const bool has_value   = type != types::kVoidType && type != types::kErrorType;
    const auto result_slot = has_value ? next_slot_++ : hir::kInvalidHirSlot;
    if (has_value)
        current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(result_slot, type));

    const auto merge_block = newBlock();
    size_t chain_block     = current_block_;
    for (size_t i = 0; i < case_count; ++i) {
        const size_t body_index = i + 1U;
        const bool is_last      = i + 1U == case_count;
        const bool is_default   = i < expr.conditions.size() && !expr.conditions[i];

        if (i > 0U) {
            setCurrentBlock(chain_block);
            current_fn_->blocks[chain_block].insts = memory::DynArray<hir::HirExprId>(arena_);
        }

        const auto body_block = newBlock();
        if (is_default) {
            emitJump(body_block);
            if (!is_last)
                chain_block = newBlock(); // unreachable continuation for a misplaced default
        } else {
            const size_t else_block = is_last ? merge_block : newBlock();
            const auto condition =
                lowerWhenCondition(expr.conditions[i], subject_slot, subject_type);
            hir::HirBranch branch;
            branch.cond       = condition;
            branch.then_block = static_cast<hir::HirDeclId>(body_block);
            branch.else_block = static_cast<hir::HirDeclId>(else_block);
            setTerminator(addExpr(std::move(branch)));
            chain_block = else_block;
        }

        setCurrentBlock(body_block);
        current_fn_->blocks[body_block].insts = memory::DynArray<hir::HirExprId>(arena_);
        // An `(f is Member)` case narrows reads of `f` to the member type.
        frontend::LocalId narrowed_local = {};
        types::TypeId narrowed_type      = types::kInvalidType;
        bool narrowed_opaque_payload     = false;
        if (i < expr.conditions.size() && expr.conditions[i]) {
            const auto &condition =
                current_module_->frontend->expressions()[expr.conditions[i].value - 1U];
            if (condition.kind == frontend::ExprKind::IsType && !condition.operands.empty() &&
                condition.cast_type) {
                const auto *resolved = findResolvedExpr(condition.operands[0]);
                if (resolved != nullptr && resolved->local) {
                    narrowed_local = resolved->local;
                    narrowed_type  = lowerType(sema_.typeTable().lowerTypeExpr(
                        *current_module_->frontend, condition.cast_type));
                    const sema::modern::TypeId local_sema = semaTypeOfLocal(resolved->local);
                    narrowed_opaque_payload =
                        sema_.typeTable().kindOf(sema_.typeTable().stripQualifiers(local_sema)) ==
                        TypeKind::Opaque;
                }
            }
        }
        if (narrowed_local) {
            narrowing_stack_.push_back(Narrowing{
                narrowed_local, narrowed_type, /*optionalPayload=*/false, narrowed_opaque_payload});
        }
        const auto body_value = lowerExpr(expr.operands[body_index]);
        if (narrowed_local) {
            narrowing_stack_.pop_back();
        }
        if (has_value && body_value != hir::kInvalidHirExpr &&
            current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr) {
            current_fn_->blocks[current_block_].insts.push(emitSlotStore(result_slot, body_value));
        }
        if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr)
            emitJump(merge_block);
    }

    setCurrentBlock(merge_block);
    current_fn_->blocks[merge_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    return has_value ? emitSlotLoad(result_slot, type) : hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerWhenCondition(frontend::ExprId condition,
                                                  const hir::HirSlotId subject_slot,
                                                  const types::TypeId subject_type) {
    if (!condition || current_module_ == nullptr || current_module_->frontend == nullptr ||
        condition.value > current_module_->frontend->expressions().size())
        return hir::kInvalidHirExpr;
    const auto &node = current_module_->frontend->expressions()[condition.value - 1U];
    if (node.kind == frontend::ExprKind::Range) {
        const auto lower_bound = lowerExpr(node.operands[0]);
        const auto upper_bound = lowerExpr(node.operands[1]);
        const auto subject     = emitSlotLoad(subject_slot, subject_type);
        if (lower_bound == hir::kInvalidHirExpr || upper_bound == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;

        hir::HirBinary ge;
        ge.lhs           = subject;
        ge.rhs           = lower_bound;
        ge.op            = hir::HirBinaryOp::Ge;
        ge.type          = types::kBoolType;
        const auto ge_id = addExpr(std::move(ge));

        hir::HirBinary le;
        le.lhs           = subject;
        le.rhs           = upper_bound;
        le.op            = hir::HirBinaryOp::Le;
        le.type          = types::kBoolType;
        const auto le_id = addExpr(std::move(le));

        hir::HirBinary conjunction;
        conjunction.lhs  = ge_id;
        conjunction.rhs  = le_id;
        conjunction.op   = hir::HirBinaryOp::And;
        conjunction.type = types::kBoolType;
        return addExpr(std::move(conjunction));
    }

    // A boolean or optional condition is tested directly through the same
    // implicit test rule as `if`/`while`; any other condition is an equality
    // pattern (`(0)` means `subject == 0`).
    const auto condition_type = typeOfExpr(condition);
    if (types_.kindOf(condition_type) == types::TypeKind::Bool)
        return lowerExpr(condition);
    if (types_.kindOf(condition_type) == types::TypeKind::Optional)
        return lowerOptionalCondition(condition);

    const auto value   = lowerExpr(condition);
    const auto subject = emitSlotLoad(subject_slot, subject_type);
    if (value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    hir::HirBinary equality;
    equality.lhs  = subject;
    equality.rhs  = value;
    equality.op   = hir::HirBinaryOp::Eq;
    equality.type = types::kBoolType;
    return addExpr(std::move(equality));
}

hir::HirExprId HirLowerModern::lowerWhile(const frontend::Expression &expr) {
    if (expr.operands.size() < 2U)
        return hir::kInvalidHirExpr;

    const auto header_block = newBlock();
    const auto body_block   = newBlock();
    const auto exit_block   = newBlock();

    emitJump(header_block);

    setCurrentBlock(header_block);
    current_fn_->blocks[header_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto cond                         = lowerCondition(expr.operands[0]);
    hir::HirBranch branch;
    branch.cond       = cond;
    branch.then_block = static_cast<hir::HirDeclId>(body_block);
    branch.else_block = static_cast<hir::HirDeclId>(exit_block);
    setTerminator(addExpr(std::move(branch)));

    loop_stack_.push_back({header_block, exit_block, expr.label, 0U});
    setCurrentBlock(body_block);
    current_fn_->blocks[body_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    cleanup_stack_.push_back(CleanupFrame(arena_));
    loop_stack_.back().cleanup_depth = cleanup_stack_.size() - 1U;
    (void)lowerExpr(expr.operands[1]);
    emitCleanupFrom(cleanup_stack_.size() - 1U);
    cleanup_stack_.pop_back();
    // The body may have created nested control flow; the back edge belongs on the
    // block the body actually ended in (current_block_), not necessarily body_block.
    if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr)
        emitJump(header_block);
    loop_stack_.pop_back();

    setCurrentBlock(exit_block);
    current_fn_->blocks[exit_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerFor(const frontend::Expression &expr) {
    // operands: [cond, body, step]; step may be invalid.
    if (expr.operands.size() < 2U)
        return hir::kInvalidHirExpr;

    const auto header_block = newBlock();
    const auto body_block   = newBlock();
    const auto step_block   = newBlock();
    const auto exit_block   = newBlock();

    emitJump(header_block);

    setCurrentBlock(header_block);
    current_fn_->blocks[header_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto cond                         = lowerCondition(expr.operands[0]);
    hir::HirBranch branch;
    branch.cond       = cond;
    branch.then_block = static_cast<hir::HirDeclId>(body_block);
    branch.else_block = static_cast<hir::HirDeclId>(exit_block);
    setTerminator(addExpr(std::move(branch)));

    // `continue` runs the step, so the step is the continue target; `break` exits.
    loop_stack_.push_back({step_block, exit_block, expr.label, 0U});
    setCurrentBlock(body_block);
    current_fn_->blocks[body_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    cleanup_stack_.push_back(CleanupFrame(arena_));
    loop_stack_.back().cleanup_depth = cleanup_stack_.size() - 1U;
    (void)lowerExpr(expr.operands[1]);
    emitCleanupFrom(cleanup_stack_.size() - 1U);
    cleanup_stack_.pop_back();
    // The body may have created nested control flow; the step edge belongs on the
    // block the body actually ended in (current_block_), not necessarily body_block.
    if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr)
        emitJump(step_block);
    loop_stack_.pop_back();

    setCurrentBlock(step_block);
    current_fn_->blocks[step_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    if (expr.operands.size() >= 3U && expr.operands[2]) {
        const auto step = lowerExpr(expr.operands[2]);
        if (step != hir::kInvalidHirExpr)
            current_fn_->blocks[step_block].insts.push(step);
    }
    if (current_fn_->blocks[step_block].terminator == hir::kInvalidHirExpr)
        emitJump(header_block);

    setCurrentBlock(exit_block);
    current_fn_->blocks[exit_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerForIn(const frontend::Expression &expr) {
    if (expr.operands.size() < 2U)
        return hir::kInvalidHirExpr;

    // The `next` method and the two union member indexes were validated by
    // sema. If any is missing this lowering ran before a successful sema pass.
    const auto *next_ptr =
        current_types_ != nullptr ? current_types_->forInNext.get(expr.id.value) : nullptr;
    const auto *element_index_ptr =
        current_types_ != nullptr ? current_types_->forInElementIndex.get(expr.id.value) : nullptr;
    const auto *end_index_ptr =
        current_types_ != nullptr ? current_types_->forInEndIndex.get(expr.id.value) : nullptr;
    const auto *union_sema_type_ptr =
        current_types_ != nullptr ? current_types_->forInUnionType.get(expr.id.value) : nullptr;
    const auto *optional_sema_type_ptr =
        current_types_ != nullptr ? current_types_->forInOptionalType.get(expr.id.value) : nullptr;
    const bool has_optional = optional_sema_type_ptr != nullptr && !!*optional_sema_type_ptr;
    const bool has_union =
        element_index_ptr != nullptr && end_index_ptr != nullptr && union_sema_type_ptr != nullptr;
    if (next_ptr == nullptr || !next_ptr->decl || !expr.forInBinding ||
        (!has_optional && !has_union)) {
        return hir::kInvalidHirExpr;
    }

    const frontend::DeclId next_decl     = next_ptr->decl;
    const session::ModuleKey next_module = next_ptr->module;
    const auto loop_type                 = typeOfLocal(expr.forInBinding);
    if (loop_type == types::kErrorType)
        return hir::kInvalidHirExpr;

    // Keep the iterable alive for the whole loop. A value receiver is copied
    // into a slot and its address is passed as the implicit self argument; a
    // pointer receiver can pass the loaded value directly.
    const auto iterable = lowerExpr(expr.operands[0]);
    if (iterable == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto iterable_type                 = typeOfExpr(expr.operands[0]);
    const sema::modern::TypeId sema_iterable = semaTypeOfExpr(expr.operands[0]);
    const auto iterable_slot                 = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(iterable_slot, iterable_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(iterable_slot, iterable));
    const auto iterable_addr = addExpr(hir::HirSlotAddr{iterable_slot, iterable_type});

    const auto makeMethodCall = [&](const session::ModuleKey &decl_module,
                                    const frontend::DeclId decl_id) -> hir::HirExprId {
        if (decl_module.empty() || !decl_id)
            return hir::kInvalidHirExpr;
        const auto *module_artifact = snapshot_.findModule(decl_module);
        const auto *method_decl =
            module_artifact != nullptr ? findDecl(*module_artifact, decl_id) : nullptr;
        if (module_artifact == nullptr || method_decl == nullptr)
            return hir::kInvalidHirExpr;
        const auto key             = internFunctionKey(interner_, module_artifact->key, decl_id);
        const auto *function_index = function_index_by_key_.get(key);
        if (function_index == nullptr)
            return hir::kInvalidHirExpr;

        memory::DynArray<hir::HirExprId> args(arena_);
        memory::DynArray<types::TypeId> arg_types(arena_);
        (void)sema_iterable;
        const auto self_type = iterable_type;
        args.push(iterable_addr);
        arg_types.push(self_type);

        hir::HirCall call{hir::kInvalidHirExpr, std::move(args), std::move(arg_types)};
        call.resolved_fn = functions_[*function_index].sym_id;
        return addExpr(std::move(call));
    };

    const auto header_block = newBlock();
    const auto body_block   = newBlock();
    const auto exit_block   = newBlock();

    emitJump(header_block);

    // Header: `let step = iter.next(); if (step is End) break;`.
    setCurrentBlock(header_block);
    current_fn_->blocks[header_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto next_call                    = makeMethodCall(next_module, next_decl);
    if (next_call == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto next_slot = next_slot_++;
    const auto next_type =
        has_optional ? lowerType(*optional_sema_type_ptr) : lowerType(*union_sema_type_ptr);
    current_fn_->blocks[header_block].insts.push(emitSlotAlloca(next_slot, next_type));
    current_fn_->blocks[header_block].insts.push(emitSlotStore(next_slot, next_call));
    hir::HirExprId cond_expr = addExpr(hir::HirMakeNone{next_type});
    if (has_optional) {
        // `next(self): ?T`: `None` is End. For aggregate optionals the tag is
        // field 1; for optional pointers the value itself is the sentinel.
        const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(next_type));
        const bool niche =
            optional != nullptr && types_.kindOf(optional->inner) == types::TypeKind::Ptr;
        if (!niche) {
            const auto next_addr = addExpr(hir::HirSlotAddr{next_slot, next_type});
            cond_expr = addExpr(hir::HirField{next_addr, 1U, types::kBoolType, next_type});
            cond_expr = addExpr(hir::HirUnary{hir::HirUnaryOp::Not, cond_expr, types::kBoolType});
        } else {
            const auto loaded = addExpr(hir::HirSlotLoad{next_slot, next_type});
            cond_expr         = addExpr(hir::HirBinary{loaded, addExpr(hir::HirMakeNone{next_type}),
                                               hir::HirBinaryOp::Eq, types::kBoolType});
        }
    } else {
        const uint32_t end_index = *end_index_ptr;
        const auto union_type    = lowerType(*union_sema_type_ptr);
        const auto *union_def    = std::get_if<types::TypeUnion>(&types_.lookup(union_type));
        const auto *def = union_def != nullptr ? types_.lookupUnionDef(union_def->def_id) : nullptr;
        if (def == nullptr || !def->is_tagged)
            return hir::kInvalidHirExpr;
        const auto tag_type = tagType(types_, static_cast<uint32_t>(def->members.size()));
        const auto tag = addExpr(hir::HirField{addExpr(hir::HirSlotAddr{next_slot, union_type}), 1U,
                                               tag_type, union_type});
        hir::HirUnionCheck check;
        check.value        = tag;
        check.union_type   = union_type;
        check.member_index = end_index;
        cond_expr          = addExpr(std::move(check));
    }
    hir::HirBranch branch;
    branch.cond       = cond_expr;
    branch.then_block = static_cast<hir::HirDeclId>(exit_block);
    branch.else_block = static_cast<hir::HirDeclId>(body_block);
    setTerminator(addExpr(std::move(branch)));

    // Body: loop = step as Element; lower the user block.
    setCurrentBlock(body_block);
    current_fn_->blocks[body_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    auto payload                          = emitSlotLoad(next_slot, next_type);
    if (has_optional) {
        // Optional payload extraction: field 0 for aggregate `?T`; the value
        // itself for nullable pointers. `??T` extracts the outer payload,
        // which is still `?T` and becomes the loop variable type directly.
        const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(next_type));
        if (optional != nullptr && types_.kindOf(optional->inner) != types::TypeKind::Ptr) {
            payload = addExpr(hir::HirField{addExpr(hir::HirSlotAddr{next_slot, next_type}), 0U,
                                            loop_type, next_type});
        }
    } else {
        hir::HirUnionCast cast;
        cast.value        = payload;
        cast.from         = lowerType(*union_sema_type_ptr);
        cast.to           = loop_type;
        cast.member_index = *element_index_ptr;
        cast.checked      = false;
        payload           = addExpr(std::move(cast));
    }
    const auto loop_slot = localSlot(expr.forInBinding);
    current_fn_->blocks[body_block].insts.push(emitSlotAlloca(loop_slot, loop_type));
    current_fn_->blocks[body_block].insts.push(emitSlotStore(loop_slot, payload));

    // `continue` re-tests by jumping to the header, which calls `next` again.
    loop_stack_.push_back({header_block, exit_block, expr.label, 0U});
    const frontend::StmtId saved_for_in_stmt = current_for_in_binding_stmt_;
    current_for_in_binding_stmt_             = expr.forInBindingStmt;
    current_for_in_binding_local_            = expr.forInBinding;
    cleanup_stack_.push_back(CleanupFrame(arena_));
    loop_stack_.back().cleanup_depth = cleanup_stack_.size() - 1U;
    (void)lowerExpr(expr.operands[1]);
    emitCleanupFrom(cleanup_stack_.size() - 1U);
    cleanup_stack_.pop_back();
    current_for_in_binding_stmt_  = saved_for_in_stmt;
    current_for_in_binding_local_ = {};
    if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr)
        emitJump(header_block);
    loop_stack_.pop_back();

    setCurrentBlock(exit_block);
    current_fn_->blocks[exit_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerAssign(const frontend::Expression &expr,
                                           const types::TypeId type) {
    (void)type;
    if (expr.operands.size() != 2U)
        return hir::kInvalidHirExpr;

    const auto &lhs_expr = current_module_->frontend->expressions()[expr.operands[0].value - 1U];
    if (lhs_expr.kind == frontend::ExprKind::Name) {
        if (const auto *resolved = findResolvedExpr(expr.operands[0]);
            resolved != nullptr && resolved->local) {
            const auto value = lowerExpr(expr.operands[1]);
            if (value == hir::kInvalidHirExpr)
                return hir::kInvalidHirExpr;
            const auto value_slice =
                lowerCoerceToTarget(typeOfLocal(resolved->local), expr.operands[1], value);
            return emitSlotStore(localSlot(resolved->local), value_slice);
        }
    }
    // For field/arrow lvalue targets, lower the lhs normally (produces HirField) then assign.
    if (lhs_expr.kind == frontend::ExprKind::Field || lhs_expr.kind == frontend::ExprKind::Arrow ||
        lhs_expr.kind == frontend::ExprKind::Index) {
        const auto target_type = typeOfExpr(expr.operands[0]);
        const auto target =
            lhs_expr.kind == frontend::ExprKind::Field   ? lowerField(lhs_expr, target_type)
            : lhs_expr.kind == frontend::ExprKind::Arrow ? lowerArrow(lhs_expr, target_type)
                                                         : lowerIndex(lhs_expr, target_type);
        const auto value = lowerExpr(expr.operands[1]);
        if (target == hir::kInvalidHirExpr || value == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        const auto value_slice = lowerCoerceToTarget(target_type, expr.operands[1], value);
        hir::HirAssign assign;
        assign.target = target;
        assign.value  = value_slice;
        return addExpr(std::move(assign));
    }

    const auto target = lowerExpr(expr.operands[0]);
    const auto value  = lowerExpr(expr.operands[1]);
    if (target == hir::kInvalidHirExpr || value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto value_slice =
        lowerCoerceToTarget(typeOfExpr(expr.operands[0]), expr.operands[1], value);

    hir::HirAssign assign;
    assign.target = target;
    assign.value  = value_slice;
    return addExpr(std::move(assign));
}

hir::HirExprId HirLowerModern::lowerOptionalProp(const frontend::Expression &expr,
                                                 const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    const auto operand = lowerExpr(expr.operands[0]);
    if (operand == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    const auto operand_type = typeOfExpr(expr.operands[0]);
    if (types_.kindOf(operand_type) != types::TypeKind::Optional)
        return hir::kInvalidHirExpr;

    // Spill the optional so its payload and tag can be addressed as fields.
    const auto slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, operand_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, operand));

    // The result of `x?` is stored here so both paths agree on one location.
    const auto result_slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(result_slot, type));

    const auto is_some = addExpr(hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 1U,
                                               types::kBoolType, operand_type});

    const auto some_block = newBlock();
    const auto none_block = newBlock();

    hir::HirBranch branch;
    branch.cond       = is_some;
    branch.then_block = static_cast<hir::HirDeclId>(some_block);
    branch.else_block = static_cast<hir::HirDeclId>(none_block);
    setTerminator(addExpr(std::move(branch)));

    // None: propagate by returning None from the enclosing function.
    setCurrentBlock(none_block);
    current_fn_->blocks[none_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    {
        hir::HirRet ret;
        hir::HirMakeNone make_none;
        make_none.type = current_fn_->return_type;
        ret.value      = addExpr(std::move(make_none));
        setTerminator(addExpr(std::move(ret)));
    }

    // Some: unwrap the payload into the result slot and carry on.
    setCurrentBlock(some_block);
    current_fn_->blocks[some_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto payload                    = addExpr(
        hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 0U, type, operand_type});
    current_fn_->blocks[some_block].insts.push(emitSlotStore(result_slot, payload));
    return emitSlotLoad(result_slot, type);
}

hir::HirExprId HirLowerModern::lowerCondition(frontend::ExprId condition) {
    if (!condition)
        return hir::kInvalidHirExpr;
    const auto condition_type = typeOfExpr(condition);
    if (types_.kindOf(condition_type) == types::TypeKind::Optional)
        return lowerOptionalCondition(condition);
    return lowerExpr(condition);
}

hir::HirExprId HirLowerModern::lowerOptionalCondition(frontend::ExprId id) {
    if (!id)
        return hir::kInvalidHirExpr;
    const auto operand      = lowerExpr(id);
    const auto operand_type = typeOfExpr(id);
    if (operand == hir::kInvalidHirExpr || types_.kindOf(operand_type) != types::TypeKind::Optional)
        return hir::kInvalidHirExpr;

    const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(operand_type));
    const bool niche =
        optional != nullptr && types_.kindOf(optional->inner) == types::TypeKind::Ptr;
    if (niche) {
        // ?*T uses nullptr as the None sentinel; non-null is the owned boolean.
        hir::HirMakeNone none;
        none.type = operand_type;
        return addExpr(hir::HirBinary{operand, addExpr(std::move(none)), hir::HirBinaryOp::Ne,
                                      types::kBoolType});
    }

    // {payload, tag} layout: spill, read the tag (index 1), use it directly as bool.
    const auto slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, operand_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, operand));
    return addExpr(hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 1U,
                                 types::kBoolType, operand_type});
}

/// Lowers extraction of an optional payload. When `checked` is true the None
/// branch terminates through `R10003`; false reads the payload unconditionally.
hir::HirExprId HirLowerModern::lowerOptionalPayload(const frontend::Expression &expr,
                                                    const types::TypeId type, bool checked) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    const auto operand      = lowerExpr(expr.operands[0]);
    const auto operand_type = typeOfExpr(expr.operands[0]);
    if (operand == hir::kInvalidHirExpr || types_.kindOf(operand_type) != types::TypeKind::Optional)
        return hir::kInvalidHirExpr;
    if (!checked) {
        const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(operand_type));
        if (optional != nullptr && types_.kindOf(optional->inner) == types::TypeKind::Ptr)
            return operand;
        const auto slot = next_slot_++;
        current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, operand_type));
        current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, operand));
        return addExpr(
            hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 0U, type, operand_type});
    }

    const auto slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, operand_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, operand));

    const auto is_some = [&]() -> hir::HirExprId {
        const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(operand_type));
        if (optional != nullptr && types_.kindOf(optional->inner) == types::TypeKind::Ptr) {
            hir::HirMakeNone none;
            none.type = operand_type;
            return addExpr(hir::HirBinary{addExpr(hir::HirSlotLoad{slot, operand_type}),
                                          addExpr(std::move(none)), hir::HirBinaryOp::Ne,
                                          types::kBoolType});
        }
        return addExpr(hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 1U,
                                     types::kBoolType, operand_type});
    };

    const auto some_block = newBlock();
    const auto none_block = newBlock();
    hir::HirBranch branch;
    branch.cond       = is_some();
    branch.then_block = static_cast<hir::HirDeclId>(some_block);
    branch.else_block = static_cast<hir::HirDeclId>(none_block);
    setTerminator(addExpr(std::move(branch)));

    setCurrentBlock(none_block);
    current_fn_->blocks[none_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    hir::HirRuntimePanic panic;
    panic.code = 10003U;
    setTerminator(addExpr(std::move(panic)));

    setCurrentBlock(some_block);
    current_fn_->blocks[some_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(operand_type));
    if (optional != nullptr && types_.kindOf(optional->inner) == types::TypeKind::Ptr)
        return emitSlotLoad(slot, operand_type);
    return addExpr(
        hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 0U, type, operand_type});
}

hir::HirExprId HirLowerModern::lowerMust(const frontend::Expression &expr,
                                         const types::TypeId type) {
    return lowerOptionalPayload(expr, type, /*checked=*/true);
}

hir::HirExprId HirLowerModern::lowerRawOptional(const frontend::Expression &expr,
                                                const types::TypeId type) {
    return lowerOptionalPayload(expr, type, /*checked=*/false);
}

hir::HirExprId HirLowerModern::lowerCast(const frontend::Expression &expr,
                                         const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    auto value = lowerExpr(expr.operands[0]);
    if (value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    auto from = typeOfExpr(expr.operands[0]);
    if (current_module_ != nullptr &&
        (sema_.isSelfReceiver(current_module_->key, expr.operands[0]) ||
         sema_.isBorrowParameter(current_module_->key, expr.operands[0]))) {
        const auto sema_from = sema_.typeTable().stripQualifiers(semaTypeOfExpr(expr.operands[0]));
        const auto *ptr      = sema_.typeTable().pointer(sema_from);
        if (ptr != nullptr) {
            const auto pointee     = sema_.typeTable().stripQualifiers(ptr->pointee);
            const auto pointee_hir = lowerType(pointee);
            if (pointee_hir != types::kInvalidType && pointee_hir != types::kErrorType) {
                from  = pointee_hir;
                value = addExpr(hir::HirUnary{hir::HirUnaryOp::Deref, value, pointee_hir});
            }
        }
    }
    if (from == type)
        return value;
    const bool from_opaque = types_.kindOf(from) == types::TypeKind::OpaqueTagged;
    const bool to_opaque   = types_.kindOf(type) == types::TypeKind::OpaqueTagged;

    // Bare `opaque` is handled before nominal/union casts: `T as opaque` must
    // erase the whole value, not treat a one-field aggregate as a wrapper.
    if (to_opaque) {
        if (current_module_ == nullptr || current_module_->key != snapshot_.rootModuleKey()) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::UnsupportedSyntax,
                          "bare 'opaque' type ids are module-local; imported or cached "
                          "'opaque' values are not supported in this version",
                          current_module_ != nullptr ? memory::Span{current_module_->fileId,
                                                                    expr.span.start, expr.span.end}
                                                     : memory::Span{});
            return hir::kInvalidHirExpr;
        }
        hir::HirMakeOpaque make;
        make.value       = value;
        make.source_type = from;
        make.opaque_type = type;
        make.type_id     = stableConcreteTypeId(from);
        return addExpr(std::move(make));
    }

    // Nominal casts (`T as Nominal` / `Nominal as T`) are lowered as the
    // one-field wrapper's literal/extraction, not as numeric conversions.
    if (types_.kindOf(type) == types::TypeKind::Struct && from != type) {
        const auto wrapper_count = types_.fieldCount(type);
        if (wrapper_count == 1U) {
            hir::HirStructLiteral literal(arena_);
            literal.type = type;
            literal.values.push(value);
            return addExpr(std::move(literal));
        }
    }
    if (types_.kindOf(from) == types::TypeKind::Struct && from != type) {
        const auto wrapper_count = types_.fieldCount(from);
        if (wrapper_count == 1U)
            return addExpr(hir::HirField{value, 0U, type, from});
    }

    // `opaque as T` lowers to a checked optional extraction. The HIR cast keeps
    // the type id so codegen can branch on it and emit Some/None payloads.
    if (from_opaque && !to_opaque) {
        if (current_module_ == nullptr || current_module_->key != snapshot_.rootModuleKey()) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::UnsupportedSyntax,
                          "bare 'opaque' type ids are module-local; imported or cached "
                          "'opaque' values are not supported in this version",
                          current_module_ != nullptr ? memory::Span{current_module_->fileId,
                                                                    expr.span.start, expr.span.end}
                                                     : memory::Span{});
            return hir::kInvalidHirExpr;
        }

        // `opaque as raw opaque` is the explicit unchecked way to reinterpret a
        // bare opaque as the `void*` it stores: it is a pointer extraction, not a
        // load of a concrete T payload.
        if (types_.kindOf(type) == types::TypeKind::Ptr) {
            const auto *ptr = std::get_if<types::TypePtr>(&types_.lookup(type));
            if (ptr != nullptr && ptr->pointee != types::kInvalidType &&
                types_.kindOf(ptr->pointee) == types::TypeKind::Void) {
                hir::HirOpaqueCast cast;
                cast.value       = value;
                cast.from        = from;
                cast.to          = type;
                cast.checked     = false;
                cast.type_id     = stableConcreteTypeId(from);
                cast.opaque_type = from;
                cast.result_type = type;
                cast.returns_ptr = true;
                return addExpr(std::move(cast));
            }
        }

        if (expr.is_raw) {
            hir::HirOpaqueCast cast;
            cast.value       = value;
            cast.from        = from;
            cast.to          = type;
            cast.checked     = false;
            cast.type_id     = stableConcreteTypeId(type);
            cast.opaque_type = from;
            cast.result_type = type;
            return addExpr(std::move(cast));
        }

        // Sema reports the checked extraction as `?T`, where `T` is the cast
        // target written by the user. The opaque tag was recorded for the
        // concrete payload type, so unwrap the optional here instead of hashing
        // the optional TypeId.
        const auto *optional_result = std::get_if<types::TypeOptional>(&types_.lookup(type));
        const auto payload_type     = optional_result != nullptr ? optional_result->inner : type;
        const auto result_type      = types_.internOptional(payload_type);
        const auto value_slot       = next_slot_++;
        const auto result_slot      = next_slot_++;
        current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(value_slot, from));
        current_fn_->blocks[current_block_].insts.push(emitSlotStore(value_slot, value));
        current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(result_slot, result_type));

        hir::HirOpaqueCheck check;
        check.value         = emitSlotLoad(value_slot, from);
        check.opaque_type   = from;
        check.type_id       = stableConcreteTypeId(payload_type);
        const auto check_id = addExpr(std::move(check));

        const auto some_block  = newBlock();
        const auto none_block  = newBlock();
        const auto merge_block = newBlock();
        hir::HirBranch branch;
        branch.cond       = check_id;
        branch.then_block = static_cast<hir::HirDeclId>(some_block);
        branch.else_block = static_cast<hir::HirDeclId>(none_block);
        setTerminator(addExpr(std::move(branch)));

        setCurrentBlock(some_block);
        current_fn_->blocks[some_block].insts = memory::DynArray<hir::HirExprId>(arena_);
        hir::HirOpaqueCast cast;
        cast.value         = emitSlotLoad(value_slot, from);
        cast.from          = from;
        cast.to            = payload_type;
        cast.checked       = false;
        cast.type_id       = stableConcreteTypeId(payload_type);
        cast.opaque_type   = from;
        cast.result_type   = payload_type;
        const auto payload = addExpr(std::move(cast));
        hir::HirMakeSome some;
        some.type  = result_type;
        some.value = payload;
        current_fn_->blocks[some_block].insts.push(
            emitSlotStore(result_slot, addExpr(std::move(some))));
        emitJump(merge_block);

        setCurrentBlock(none_block);
        current_fn_->blocks[none_block].insts = memory::DynArray<hir::HirExprId>(arena_);
        hir::HirMakeNone none;
        none.type = result_type;
        current_fn_->blocks[none_block].insts.push(
            emitSlotStore(result_slot, addExpr(std::move(none))));
        emitJump(merge_block);

        setCurrentBlock(merge_block);
        current_fn_->blocks[merge_block].insts = memory::DynArray<hir::HirExprId>(arena_);
        return emitSlotLoad(result_slot, result_type);
    }

    if (types_.kindOf(from) == types::TypeKind::Union &&
        types_.kindOf(type) != types::TypeKind::Union) {
        const auto *union_type = std::get_if<types::TypeUnion>(&types_.lookup(from));
        const auto *def =
            union_type != nullptr ? types_.lookupUnionDef(union_type->def_id) : nullptr;
        if (def != nullptr && def->is_tagged) {
            hir::HirUnionCast cast;
            cast.value        = value;
            cast.from         = from;
            cast.to           = type;
            cast.member_index = taggedMemberIndex(from, type);
            cast.checked      = !expr.is_raw;
            return addExpr(std::move(cast));
        }
    }
    if (types_.kindOf(from) == types::TypeKind::Union ||
        types_.kindOf(type) == types::TypeKind::Union)
        return addExpr(hir::HirUnionCast{value, from, type});

    return addExpr(hir::HirCast{value, from, type});
}

uint32_t HirLowerModern::stableConcreteTypeId(types::TypeId type) const {
    uint32_t hash     = 2166136261U;
    const auto append = [&](const uint8_t *bytes, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            hash ^= bytes[i];
            hash *= 16777619U;
        }
    };
    auto appendU64 = [&](uint64_t value) {
        const uint8_t raw[sizeof(value)] = {
            static_cast<uint8_t>(value),        static_cast<uint8_t>(value >> 8U),
            static_cast<uint8_t>(value >> 16U), static_cast<uint8_t>(value >> 24U),
            static_cast<uint8_t>(value >> 32U), static_cast<uint8_t>(value >> 40U),
            static_cast<uint8_t>(value >> 48U), static_cast<uint8_t>(value >> 56U)};
        append(raw, sizeof(raw));
    };
    const auto appendName = [&](memory::InternedId name) {
        const auto text = interner_.lookup(name);
        append(reinterpret_cast<const uint8_t *>(text.data()), text.size());
    };

    const auto namespace_text =
        moduleNamespace(current_module_ != nullptr ? current_module_->key : std::string_view{},
                        snapshot_.cacheKey());
    append(reinterpret_cast<const uint8_t *>(namespace_text.data()), namespace_text.size());
    appendU64(static_cast<uint64_t>(static_cast<TypeKind>(types_.kindOf(type))));

    auto appendType = [&](const auto &self, types::TypeId current) -> void {
        const auto &data = types_.lookup(current);
        std::visit(common::overloaded{
                       [&](const types::TypeError &) { appendU64(1); },
                       [&](const types::TypeNever &) { appendU64(2); },
                       [&](const types::TypeVoid &) { appendU64(3); },
                       [&](const types::TypeBool &) { appendU64(4); },
                       [&](const types::TypeChar &) { appendU64(5); },
                       [&](const types::TypeInt &t) {
                           appendU64(6);
                           appendU64(static_cast<uint64_t>(t.width));
                       },
                       [&](const types::TypeFloat &t) {
                           appendU64(7);
                           appendU64(static_cast<uint64_t>(t.width));
                       },
                       [&](const types::TypePtr &t) {
                           appendU64(8);
                           appendU64(static_cast<uint64_t>(t.is_mut));
                           appendU64(static_cast<uint64_t>(t.ownership));
                           self(self, t.pointee);
                       },
                       [&](const types::TypeArray &t) {
                           appendU64(9);
                           appendU64(t.count);
                           self(self, t.elem);
                       },
                       [&](const types::TypeStruct &) {
                           appendU64(10);
                           const auto &def = types_.getStructDef(current);
                           appendName(def.name);
                           for (const auto &field : def.fields) {
                               appendName(field.name);
                               self(self, field.type);
                           }
                       },
                       [&](const types::TypeFn &t) {
                           appendU64(11);
                           appendU64(t.param_count);
                           self(self, t.ret);
                           for (size_t i = 0; i < t.param_count; ++i)
                               self(self, t.params[i]);
                       },
                       [&](const types::TypeTypeVar &t) {
                           appendU64(12);
                           appendU64(t.id);
                       },
                       [&](const types::TypeOptional &t) {
                           appendU64(13);
                           self(self, t.inner);
                       },
                       [&](const types::TypeFailable &t) {
                           appendU64(14);
                           self(self, t.inner);
                       },
                       [&](const types::TypeAlias &t) {
                           appendU64(15);
                           self(self, t.target);
                       },
                       [&](const types::TypeNominal &t) {
                           appendU64(16);
                           appendName(t.name);
                           self(self, t.target);
                       },
                       [&](const types::TypeTrait &t) {
                           appendU64(17);
                           appendName(t.name);
                       },
                       [&](const types::TypeDyn &t) {
                           appendU64(18);
                           appendU64(t.method_count);
                           self(self, t.target);
                       },
                       [&](const types::TypeOpaque &) { appendU64(19); },
                       [&](const types::TypeOpaqueTagged &) { appendU64(20); },
                       [&](const types::TypeUnknown &) { appendU64(21); },
                       [&](const types::TypeQualified &t) {
                           appendU64(22);
                           appendU64(static_cast<uint64_t>(t.ownership));
                           appendU64(static_cast<uint64_t>(t.isMut));
                           self(self, t.inner);
                       },
                       [&](const types::TypeSlice &t) {
                           appendU64(23);
                           self(self, t.elem);
                       },
                       [&](const types::TypeEnum &) {
                           appendU64(24);
                           const auto &def = types_.getEnumDef(current);
                           appendName(def.name);
                           self(self, def.underlying);
                           for (const auto &variant : def.variants) {
                               appendName(variant.name);
                               appendU64(static_cast<uint64_t>(variant.discriminant));
                           }
                       },
                       [&](const types::TypeUnion &) {
                           appendU64(25);
                           const auto &def = types_.getUnionDef(current);
                           appendU64(static_cast<uint64_t>(def.is_tagged));
                           appendName(def.name);
                           for (const auto &member : def.members)
                               self(self, member);
                       },
                       [&](const types::TypePack &t) {
                           appendU64(26);
                           appendU64(t.count);
                           for (size_t i = 0; i < t.count; ++i)
                               self(self, t.members[i]);
                       },
                       [&](const types::TypeSum &t) {
                           appendU64(27);
                           appendU64(t.count);
                           for (size_t i = 0; i < t.count; ++i)
                               self(self, t.members[i]);
                       },
                       [&](const types::TypeGenericParam &t) {
                           appendU64(28);
                           appendU64(t.decl_id);
                           appendU64(t.param_index);
                       },
                       [&](const types::TypeIncomplete &t) {
                           appendU64(29);
                           appendU64(t.arg_count);
                           self(self, t.base);
                           for (size_t i = 0; i < t.arg_count; ++i)
                               self(self, t.args[i]);
                       },
                   },
                   data);
    };
    appendType(appendType, type);

    const auto domain =
        moduleNamespace(current_module_ != nullptr ? current_module_->key : std::string_view{},
                        snapshot_.cacheKey());
    append(reinterpret_cast<const uint8_t *>(domain.data()), domain.size());

    return hash == 0U ? 1U : hash;
}

/// `x is null` lowers to a tag/pointer comparison; no dedicated HIR node is needed.
hir::HirExprId HirLowerModern::lowerIsNull(const frontend::Expression &expr) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    const auto operand = lowerExpr(expr.operands[0]);
    if (operand == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto operand_type = typeOfExpr(expr.operands[0]);
    if (types_.kindOf(operand_type) != types::TypeKind::Optional)
        return hir::kInvalidHirExpr;

    const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(operand_type));
    const bool niche =
        optional != nullptr && types_.kindOf(optional->inner) == types::TypeKind::Ptr;
    if (niche) {
        // ?*T uses nullptr as the None sentinel: compare against MakeNone directly.
        hir::HirMakeNone none;
        none.type = operand_type;
        return addExpr(hir::HirBinary{operand, addExpr(std::move(none)), hir::HirBinaryOp::Eq,
                                      types::kBoolType});
    }

    // {payload, tag} layout: spill, read the tag, and negate it.
    const auto slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, operand_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, operand));
    const auto tag = addExpr(hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 1U,
                                           types::kBoolType, operand_type});
    return addExpr(hir::HirUnary{hir::HirUnaryOp::Not, tag, types::kBoolType});
}

hir::HirExprId HirLowerModern::lowerIsType(const frontend::Expression &expr) {
    if (expr.operands.empty() || !expr.cast_type || current_module_ == nullptr ||
        current_module_->frontend == nullptr)
        return hir::kInvalidHirExpr;
    auto operand = lowerExpr(expr.operands[0]);
    if (operand == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    auto operand_type = typeOfExpr(expr.operands[0]);
    if ((sema_.isSelfReceiver(current_module_->key, expr.operands[0]) ||
         sema_.isBorrowParameter(current_module_->key, expr.operands[0]))) {
        const auto sema_operand =
            sema_.typeTable().stripQualifiers(semaTypeOfExpr(expr.operands[0]));
        const auto *ptr = sema_.typeTable().pointer(sema_operand);
        if (ptr != nullptr) {
            const auto pointee     = sema_.typeTable().stripQualifiers(ptr->pointee);
            const auto pointee_hir = lowerType(pointee);
            if (pointee_hir != types::kInvalidType && pointee_hir != types::kErrorType) {
                operand      = addExpr(hir::HirUnary{hir::HirUnaryOp::Deref, operand, pointee_hir});
                operand_type = pointee_hir;
            }
        }
    }
    if (types_.kindOf(operand_type) == types::TypeKind::OpaqueTagged) {
        const auto target = lowerType(lowerTypeExprConcrete(expr.cast_type));
        if (target == types::kErrorType || target == types::kInvalidType)
            return hir::kInvalidHirExpr;
        hir::HirOpaqueCheck check;
        check.value       = operand;
        check.opaque_type = operand_type;
        check.type_id     = stableConcreteTypeId(target);
        return addExpr(std::move(check));
    }
    if (types_.kindOf(operand_type) != types::TypeKind::Union)
        return hir::kInvalidHirExpr;
    const auto *union_type = std::get_if<types::TypeUnion>(&types_.lookup(operand_type));
    if (union_type == nullptr)
        return hir::kInvalidHirExpr;
    const auto *def = types_.lookupUnionDef(union_type->def_id);
    if (def == nullptr || !def->is_tagged)
        return hir::kInvalidHirExpr;
    const auto target = lowerType(lowerTypeExprConcrete(expr.cast_type));
    if (target == types::kErrorType || target == types::kInvalidType)
        return hir::kInvalidHirExpr;
    uint32_t member_index = 0;
    for (const auto member : def->members) {
        if (member == target)
            break;
        ++member_index;
    }
    if (member_index >= def->members.size())
        return hir::kInvalidHirExpr;

    const auto slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, operand_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, operand));
    const auto tag_type = tagType(types_, static_cast<uint32_t>(def->members.size()));
    const auto tag      = addExpr(
        hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 1U, tag_type, operand_type});
    hir::HirUnionCheck check;
    check.value        = tag;
    check.union_type   = operand_type;
    check.member_index = member_index;
    return addExpr(std::move(check));
}

hir::HirExprId HirLowerModern::lowerLayoutIntrinsic(const frontend::Expression &expr) {
    hir::HirLayoutIntrinsic intrinsic;
    if (expr.text == "lengthOf" || expr.text == "ptrOf") {
        if (expr.operands.empty())
            return hir::kInvalidHirExpr;
        if (expr.text == "lengthOf") {
            // Decode escapes once so @lengthOf on a string literal returns the
            // in-memory character count rather than source bytes plus quotes.
            std::string decoded;
            std::string_view text(
                expr.operands[0].value <= current_module_->frontend->expressions().size()
                    ? current_module_->frontend->expressions()[expr.operands[0].value - 1U].text
                    : std::string_view{});
            if (!text.empty() && text.front() == '"' && text.back() == '"')
                (void)decodeEscapes(std::string_view(text.data() + 1U, text.size() - 2U), decoded);
            intrinsic.string_length = decoded.empty() ? 0 : decoded.size();
        }
        intrinsic.which        = expr.text == "lengthOf" ? hir::HirLayoutIntrinsic::Which::LengthOf
                                                         : hir::HirLayoutIntrinsic::Which::PtrOf;
        intrinsic.operand      = lowerExpr(expr.operands[0]);
        intrinsic.operand_type = typeOfExpr(expr.operands[0]);
        intrinsic.type         = typeOfExpr(expr.id);
        if (intrinsic.operand == hir::kInvalidHirExpr || intrinsic.type == types::kErrorType ||
            intrinsic.type == types::kInvalidType)
            return hir::kInvalidHirExpr;
        return addExpr(std::move(intrinsic));
    }
    if (expr.text == "sizeOf")
        intrinsic.which = hir::HirLayoutIntrinsic::Which::SizeOf;
    else if (expr.text == "alignOf")
        intrinsic.which = hir::HirLayoutIntrinsic::Which::AlignOf;
    else
        intrinsic.which = hir::HirLayoutIntrinsic::Which::OffsetOf;
    if (current_module_ == nullptr || current_module_->frontend == nullptr || !expr.cast_type)
        return hir::kInvalidHirExpr;
    const sema::modern::TypeId sema_type = lowerTypeExprConcrete(expr.cast_type);
    if (!sema_type)
        return hir::kInvalidHirExpr;
    const types::TypeId struct_type = lowerType(sema_type);
    if (struct_type == types::kErrorType)
        return hir::kInvalidHirExpr;
    intrinsic.type = struct_type;
    if (!expr.field_names.empty()) {
        const size_t field_index = types_.fieldIndex(struct_type, expr.field_names[0]);
        if (field_index == static_cast<size_t>(-1))
            return hir::kInvalidHirExpr;
        intrinsic.field_index = static_cast<uint32_t>(field_index);
    }
    return addExpr(std::move(intrinsic));
}

hir::HirExprId HirLowerModern::lowerIndex(const frontend::Expression &expr,
                                          const types::TypeId type) {
    if (expr.operands.size() < 2U)
        return hir::kInvalidHirExpr;
    const auto object = lowerExpr(expr.operands[0]);
    const auto index  = lowerExpr(expr.operands[1]);
    if (object == hir::kInvalidHirExpr || index == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    const auto object_type = typeOfExpr(expr.operands[0]);
    const auto sema_object = sema_.typeTable().stripQualifiers(semaTypeOfExpr(expr.operands[0]));
    if (sema_.typeTable().kindOf(sema_object) == sema::modern::TypeKind::Pack) {
        const auto *pack = sema_.typeTable().pack(sema_object);
        if (pack != nullptr) {
            int64_t index_value     = 0;
            const auto *module_sema = sema_.findModuleSema(current_module_->key);
            if (module_sema == nullptr ||
                !module_sema->constantIntegerValue(expr.operands[1], index_value) ||
                index_value < 0 || static_cast<uint64_t>(index_value) >= pack->members.size())
                return hir::kInvalidHirExpr;
            return addExpr(
                hir::HirField{object, static_cast<uint32_t>(index_value), type, object_type});
        }
    }
    hir::HirIndex indexing;
    indexing.object   = object;
    indexing.index    = index;
    indexing.type     = type;
    indexing.obj_type = object_type;
    indexing.is_array = types_.kindOf(object_type) == types::TypeKind::Array;
    if (expr.is_raw)
        return addExpr(std::move(indexing));

    const auto *optional    = std::get_if<types::TypeOptional>(&types_.lookup(type));
    const auto element_type = optional != nullptr ? optional->inner : type;
    const auto index_type   = typeOfExpr(expr.operands[1]);
    if (optional == nullptr)
        return addExpr(std::move(indexing));

    // Evaluate the object and index once, then branch on the dynamic bounds checks.
    const auto object_slot = next_slot_++;
    const auto index_slot  = next_slot_++;
    const auto result_slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(object_slot, object_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(object_slot, object));
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(
        index_slot,
        types_.kindOf(index_type) == types::TypeKind::Int ? index_type : types::kErrorType));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(index_slot, index));
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(result_slot, type));

    const auto loaded_index = emitSlotLoad(index_slot, index_type);
    hir::HirLiteral zero;
    zero.type          = index_type;
    zero.i             = 0;
    hir::HirExprId len = hir::kInvalidHirExpr;
    const auto *array  = std::get_if<types::TypeArray>(&types_.lookup(object_type));
    if (array != nullptr) {
        hir::HirLiteral len_lit;
        len_lit.type = index_type;
        len_lit.i    = static_cast<int64_t>(array->count);
        len          = addExpr(std::move(len_lit));
    } else {
        const auto loaded = emitSlotLoad(object_slot, object_type);
        auto len_i64 =
            addExpr(hir::HirField{loaded, 1U, types_.internInt(types::IntWidth::I64), object_type});
        const auto len64 = types_.internInt(types::IntWidth::I64);
        if (types_.kindOf(index_type) != types::TypeKind::Int) {
            len = hir::kInvalidHirExpr;
        } else if (index_type != len64) {
            len = addExpr(hir::HirCast{len_i64, len64, index_type});
        } else {
            len = len_i64;
        }
    }
    if (len == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    hir::HirBinary ge_zero;
    ge_zero.lhs           = loaded_index;
    ge_zero.rhs           = addExpr(std::move(zero));
    ge_zero.op            = hir::HirBinaryOp::Ge;
    ge_zero.type          = types::kBoolType;
    ge_zero.operand_type  = index_type;
    const auto ge_zero_id = addExpr(std::move(ge_zero));

    hir::HirBinary lt_len;
    lt_len.lhs           = loaded_index;
    lt_len.rhs           = len;
    lt_len.op            = hir::HirBinaryOp::Lt;
    lt_len.type          = types::kBoolType;
    lt_len.operand_type  = index_type;
    const auto lt_len_id = addExpr(std::move(lt_len));

    hir::HirBinary all_ok;
    all_ok.lhs          = ge_zero_id;
    all_ok.rhs          = lt_len_id;
    all_ok.op           = hir::HirBinaryOp::And;
    all_ok.type         = types::kBoolType;
    all_ok.operand_type = types::kBoolType;

    const auto some_block  = newBlock();
    const auto none_block  = newBlock();
    const auto merge_block = newBlock();
    hir::HirBranch branch;
    branch.cond       = addExpr(std::move(all_ok));
    branch.then_block = static_cast<hir::HirDeclId>(some_block);
    branch.else_block = static_cast<hir::HirDeclId>(none_block);
    setTerminator(addExpr(std::move(branch)));

    setCurrentBlock(some_block);
    current_fn_->blocks[some_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    {
        hir::HirIndex ok_index;
        ok_index.object   = emitSlotLoad(object_slot, object_type);
        ok_index.index    = emitSlotLoad(index_slot, index_type);
        ok_index.type     = element_type;
        ok_index.obj_type = object_type;
        ok_index.is_array = indexing.is_array;
        hir::HirMakeSome some;
        some.type  = type;
        some.value = addExpr(std::move(ok_index));
        current_fn_->blocks[some_block].insts.push(
            emitSlotStore(result_slot, addExpr(std::move(some))));
        emitJump(merge_block);
    }

    setCurrentBlock(none_block);
    current_fn_->blocks[none_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    {
        hir::HirMakeNone none;
        none.type = type;
        current_fn_->blocks[none_block].insts.push(
            emitSlotStore(result_slot, addExpr(std::move(none))));
        emitJump(merge_block);
    }

    setCurrentBlock(merge_block);
    current_fn_->blocks[merge_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    return emitSlotLoad(result_slot, type);
}

hir::HirExprId HirLowerModern::lowerCoerceToTarget(types::TypeId target,
                                                   frontend::ExprId expression,
                                                   hir::HirExprId value) {
    if (value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto source_type = typeOfExpr(expression);
    // Sema retypes a string literal accepted for a `[]char` target, so the
    // expression's source type is no longer `*char` here. Detect the literal
    // directly and reconstruct its pointer payload plus decoded bounds.
    if (types_.kindOf(target) == types::TypeKind::Slice && expression &&
        expression.value <= current_module_->frontend->expressions().size()) {
        const auto &literal = current_module_->frontend->expressions()[expression.value - 1U];
        if (literal.kind == frontend::ExprKind::Literal && literal.text.size() >= 2U &&
            literal.text.front() == '"' && literal.text.back() == '"') {
            const auto lowered_slice = std::get_if<types::TypeSlice>(&types_.lookup(target));
            if (lowered_slice != nullptr &&
                types_.kindOf(lowered_slice->elem) == types::TypeKind::Char) {
                const sema::modern::TypeId char_sema = sema_.typeTable().lookupNamed("char");
                const types::TypeId pointer_hir =
                    char_sema ? types_.internPtr(lowerType(char_sema)) : types::kErrorType;
                std::string decoded;
                if (pointer_hir != types::kErrorType &&
                    decodeEscapes(
                        std::string_view(literal.text.data() + 1U, literal.text.size() - 2U),
                        decoded)) {
                    hir::HirLiteral pointer_literal;
                    pointer_literal.type     = pointer_hir;
                    pointer_literal.str_val  = interner_.intern(std::string_view(decoded));
                    const auto pointer_value = addExpr(std::move(pointer_literal));
                    hir::HirMakeSlice slice;
                    slice.object      = pointer_value;
                    slice.type        = target;
                    slice.object_type = pointer_hir;
                    slice.bound_type  = types_.internInt(types::IntWidth::I64);
                    slice.is_pointer  = true;
                    hir::HirLiteral lo;
                    lo.type = slice.bound_type;
                    lo.i    = 0;
                    hir::HirLiteral hi;
                    hi.type  = slice.bound_type;
                    hi.i     = static_cast<int64_t>(decoded.size());
                    slice.lo = addExpr(std::move(lo));
                    slice.hi = addExpr(std::move(hi));
                    return addExpr(std::move(slice));
                }
            }
        }
    }
    if (types_.kindOf(source_type) != types::TypeKind::Array ||
        types_.kindOf(target) != types::TypeKind::Slice) {
        if (types_.kindOf(source_type) == types::TypeKind::Slice &&
            types_.kindOf(target) == types::TypeKind::Ptr) {
            hir::HirLayoutIntrinsic intrinsic;
            intrinsic.which        = hir::HirLayoutIntrinsic::Which::PtrOf;
            intrinsic.type         = target;
            intrinsic.operand      = value;
            intrinsic.operand_type = source_type;
            return addExpr(std::move(intrinsic));
        }
        return value;
    }

    hir::HirMakeSlice slice;
    slice.object      = value;
    slice.type        = target;
    slice.object_type = source_type;
    slice.is_array    = true;
    slice.is_pointer  = false;
    slice.checked     = false;

    const auto array      = std::get_if<types::TypeArray>(&types_.lookup(source_type));
    const auto count      = array != nullptr ? static_cast<int64_t>(array->count) : 0;
    const auto index_type = types_.internInt(types::IntWidth::I64);
    slice.bound_type      = index_type;
    hir::HirLiteral lo;
    lo.type = index_type;
    lo.i    = 0;
    hir::HirLiteral hi;
    hi.type  = index_type;
    hi.i     = count;
    slice.lo = addExpr(std::move(lo));
    slice.hi = addExpr(std::move(hi));
    return addExpr(std::move(slice));
}

hir::HirExprId
HirLowerModern::lowerVariadicSliceTail(sema::modern::TypeId slice_sema_type,
                                       const std::vector<frontend::ExprId> &tail_exprs) {
    const types::TypeId slice_type = lowerType(slice_sema_type);
    const auto *slice              = std::get_if<types::TypeSlice>(&types_.lookup(slice_type));
    if (slice == nullptr)
        return hir::kInvalidHirExpr;

    const types::TypeId element_type = slice->elem;
    const size_t tail_count          = tail_exprs.size();
    const types::TypeId array_type =
        types_.internArray(element_type, static_cast<uint32_t>(tail_count));

    hir::HirArrayLiteral array(arena_);
    array.type = array_type;
    for (size_t index = 0; index < tail_count; ++index) {
        const auto &arg_expr =
            current_module_->frontend->expressions()[tail_exprs[index].value - 1U];
        const bool annotated =
            arg_expr.kind == frontend::ExprKind::OwnershipCoerce && !arg_expr.operands.empty();
        const frontend::ExprId inner_id = annotated ? arg_expr.operands[0] : tail_exprs[index];
        auto argument                   = lowerExpr(inner_id);
        if (annotated) {
            auto address = lowerLValueAddr(inner_id);
            if (address == hir::kInvalidHirExpr) {
                const auto inner_type = typeOfExpr(inner_id);
                const auto slot       = next_slot_++;
                current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, inner_type));
                current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, argument));
                address = addExpr(hir::HirSlotAddr{slot, inner_type});
            }
            argument = address;
        }
        if (argument == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        array.elements.push(argument);
    }

    const auto array_id = addExpr(std::move(array));
    // Keep the temporary array alive for the lifetime of the slice. Callers
    // lower every argument into the current slot/block model, so the array is
    // materialised as a slot before the slice takes its address.
    const auto array_slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(array_slot, array_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(array_slot, array_id));
    const auto array_slot_addr = addExpr(hir::HirSlotAddr{array_slot, array_type});

    hir::HirMakeSlice result;
    result.type        = slice_type;
    result.object      = array_slot_addr;
    result.object_type = array_type;
    result.bound_type  = types_.internInt(types::IntWidth::I64);
    result.is_array    = true;
    result.is_pointer  = false;
    result.checked     = false;
    hir::HirLiteral lo;
    lo.type = result.bound_type;
    lo.i    = 0;
    hir::HirLiteral hi;
    hi.type   = result.bound_type;
    hi.i      = static_cast<int64_t>(tail_count);
    result.lo = addExpr(std::move(lo));
    result.hi = addExpr(std::move(hi));
    return addExpr(std::move(result));
}

hir::HirExprId HirLowerModern::lowerCoerceToDyn(sema::modern::TypeId target,
                                                frontend::ExprId expression, hir::HirExprId value,
                                                sema::modern::TypeId target_sema) {
    const types::TypeId target_hir = lowerType(target);
    if (value == hir::kInvalidHirExpr || types_.kindOf(target_hir) != types::TypeKind::Dyn ||
        current_module_ == nullptr)
        return value;

    const sema::modern::TypeId concrete_sema =
        sema_.typeTable().stripQualifiers(semaTypeOfExpr(expression));
    const auto *concrete_struct = sema_.typeTable().struct_type(concrete_sema);
    const auto *concrete_enum   = sema_.typeTable().enum_type(concrete_sema);
    const auto *concrete_union  = sema_.typeTable().union_type(concrete_sema);
    if (concrete_struct == nullptr && concrete_enum == nullptr && concrete_union == nullptr)
        return value;

    const sema::modern::TypeId target_ty = [&]() -> sema::modern::TypeId {
        const sema::modern::TypeId canonical_target =
            sema_.typeTable().stripQualifiers(sema_.typeTable().canonical(target_sema));
        const auto *target_dyn = sema_.typeTable().dyn_type(canonical_target);
        if (target_dyn != nullptr)
            return sema_.typeTable().canonical(target_dyn->target);
        return canonical_target;
    }();
    const auto *target_trait = sema_.typeTable().trait(target_ty);
    if (target_trait == nullptr)
        return value;

    const auto findDeclAcrossModules = [&](std::string_view name, frontend::DeclKind kind) {
        for (const auto &decl : current_module_->frontend->declarations()) {
            if (decl.kind == kind && decl.name == name)
                return &decl;
        }
        for (const auto &module_ptr : snapshot_.modules()) {
            if (module_ptr->key == current_module_->key || module_ptr->frontend == nullptr)
                continue;
            for (const auto &decl : module_ptr->frontend->declarations()) {
                if (decl.kind == kind && decl.name == name)
                    return &decl;
            }
        }
        return static_cast<const frontend::Declaration *>(nullptr);
    };
    const bool is_interface =
        findDeclAcrossModules(target_trait->name, frontend::DeclKind::Interface) != nullptr;
    const frontend::Declaration *dyn_decl =
        findDeclAcrossModules(target_trait->name, is_interface ? frontend::DeclKind::Interface
                                                               : frontend::DeclKind::Trait);
    if (dyn_decl == nullptr)
        return value;

    const auto concreteArgs = [&]() -> std::vector<sema::modern::TypeId> {
        std::vector<sema::modern::TypeId> args;
        if (concrete_enum != nullptr && concrete_enum->name.find('<') != std::string_view::npos) {
            const auto *template_decl =
                findDeclAcrossModules(concrete_enum->name.substr(0, concrete_enum->name.find('<')),
                                      frontend::DeclKind::Enum);
            if (template_decl == nullptr)
                return args;
            const size_t degree         = template_decl->genericParams.size();
            const std::string_view name = concrete_enum->name;
            const char *open            = name.data();
            const char *end             = open + name.size();
            const char *lt              = std::find(open, end, '<');
            const char *close           = std::find(lt + 1, end, '>');
            const char *cursor          = lt + 1;
            while (cursor < close) {
                const char *comma  = std::find(cursor, close, ',');
                const auto text    = std::string_view(cursor, static_cast<size_t>(comma - cursor));
                const auto trimmed = [text]() {
                    size_t first = 0;
                    size_t last  = text.size();
                    while (first < last && (text[first] == ' ' || text[first] == '\t'))
                        ++first;
                    while (last > first && (text[last - 1U] == ' ' || text[last - 1U] == '\t'))
                        --last;
                    return std::string_view(text.data() + first, last - first);
                }();
                args.push_back(sema_.typeTable().lookupNamed(trimmed));
                if (comma == close)
                    break;
                cursor = comma + 1;
            }
            while (args.size() < degree)
                args.push_back(sema::modern::kInvalidTypeId);
        } else if (concrete_union != nullptr) {
            args.assign(concrete_union->members.begin(), concrete_union->members.end());
        }
        return args;
    }();

    struct ReqInfo {
        const frontend::Declaration *req = nullptr;
        const frontend::Declaration *fn  = nullptr;
        session::ModuleKey req_module{};
        session::ModuleKey fn_module{};
        std::vector<sema::modern::TypeId> fn_args;
    };
    memory::DynArray<ReqInfo> requirements(arena_);
    const auto reqKey = [](const ReqInfo &info) {
        return std::make_pair(info.req_module, static_cast<uint32_t>(info.req->id.value));
    };
    const auto collectRequirement = [&](const session::ModuleArtifact &artifact) {
        for (const auto &candidate : artifact.frontend->declarations()) {
            if (candidate.kind != frontend::DeclKind::Function ||
                candidate.ownerName != target_trait->name || candidate.name.empty() ||
                candidate.name == "self")
                continue;
            bool seen = false;
            for (const auto &info : requirements)
                if (info.req != nullptr && info.req_module == artifact.key &&
                    info.req->id.value == candidate.id.value)
                    seen = true;
            if (seen)
                continue;
            ReqInfo info;
            info.req        = &candidate;
            info.req_module = artifact.key;
            requirements.push(info);
            (void)reqKey;
        }
    };
    collectRequirement(*current_module_);
    for (const auto &module_ptr : snapshot_.modules()) {
        if (module_ptr->key == current_module_->key || module_ptr->frontend == nullptr)
            continue;
        collectRequirement(*module_ptr);
    }

    std::string struct_name;
    if (concrete_struct != nullptr)
        struct_name = concrete_struct->name;
    else if (concrete_enum != nullptr)
        struct_name = concrete_enum->name;
    else if (concrete_union != nullptr)
        struct_name = concrete_union->name;
    if (const size_t angle = struct_name.find('<'); angle != std::string::npos)
        struct_name.resize(angle);

    const auto findMethod = [&](const session::ModuleArtifact &artifact, std::string_view name,
                                const frontend::Declaration **out, session::ModuleKey *out_module) {
        for (const auto &candidate : artifact.frontend->declarations()) {
            if (candidate.kind != frontend::DeclKind::Function ||
                candidate.ownerName != struct_name || candidate.name != name)
                continue;
            *out        = &candidate;
            *out_module = artifact.key;
            return true;
        }
        return false;
    };
    const auto findTraitMethod = [&](const session::ModuleArtifact &artifact, std::string_view name,
                                     const frontend::Declaration **out,
                                     session::ModuleKey *out_module) {
        for (const auto &candidate : artifact.frontend->declarations()) {
            if (candidate.kind != frontend::DeclKind::Function ||
                candidate.ownerName != target_trait->name || candidate.name != name)
                continue;
            *out        = &candidate;
            *out_module = artifact.key;
            return true;
        }
        return false;
    };
    const auto findMethodEverywhere = [&](const auto &finder, std::string_view name,
                                          const frontend::Declaration **out,
                                          session::ModuleKey *out_module) {
        if (finder(*current_module_, name, out, out_module))
            return true;
        for (const auto &module_ptr : snapshot_.modules()) {
            if (module_ptr->key == current_module_->key || module_ptr->frontend == nullptr)
                continue;
            if (finder(*module_ptr, name, out, out_module))
                return true;
        }
        return false;
    };

    for (auto &info : requirements) {
        const frontend::Declaration *concrete = nullptr;
        session::ModuleKey concrete_module{};
        // For nominal traits, an `implement Owner as Trait` method is the
        // concrete override; otherwise the trait default is the slot. For
        // structural interfaces, the concrete struct method is the slot.
        if (is_interface) {
            if (!findMethodEverywhere(findMethod, info.req->name, &concrete, &concrete_module)) {
                diags_.report(diagnostics::Severity::Error, diagnostics::err::NoMember,
                              "dyn interface implementation has no method '" + info.req->name + "'",
                              {});
                return hir::kInvalidHirExpr;
            }
        } else if (!findMethodEverywhere(findMethod, info.req->name, &concrete, &concrete_module)) {
            if (!findMethodEverywhere(findTraitMethod, info.req->name, &concrete,
                                      &concrete_module)) {
                diags_.report(diagnostics::Severity::Error, diagnostics::err::NoMember,
                              "dyn target implementation has no method '" + info.req->name + "'",
                              {});
                return hir::kInvalidHirExpr;
            }
        }
        info.fn        = concrete;
        info.fn_module = concrete_module;
        info.fn_args   = concreteArgs;
    }

    const std::string vtable_name =
        "_zith.vtable." + std::string(target_trait->name) + "." + struct_name;
    const memory::InternedId vtable_id = interner_.intern(vtable_name);
    hir::HirVTable *vtable             = nullptr;
    for (size_t vi = 0; vi < hir_.getVTableCount(); ++vi)
        if (hir_.getVTable(vi).name == vtable_id) {
            vtable = const_cast<hir::HirVTable *>(&hir_.getVTable(vi));
            break;
        }
    if (vtable == nullptr) {
        auto &added = hir_.addVTable(vtable_id);
        for (const auto &info : requirements) {
            if (info.fn == nullptr)
                continue;
            symbols::SymId slot_sym = symbols::kInvalidSym;
            const auto key          = internFunctionKey(interner_, info.fn_module, info.fn->id);
            if (auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
                size_t instance_index = ~size_t{0};
                for (size_t index = 0; index < instantiations->instanceCount(); ++index) {
                    const auto *instance = instantiations->at(index);
                    if (instance != nullptr && instance->module == info.fn_module &&
                        instance->decl == info.fn->id && instance->args == info.fn_args) {
                        instance_index = index;
                        break;
                    }
                }
                if (instance_index == ~size_t{0} && !info.fn_args.empty()) {
                    const auto *module_sema = sema_.findModuleSema(info.fn_module);
                    if (module_sema != nullptr) {
                        const auto *fn_sema =
                            sema_.typeTable().function(module_sema->typeOfDecl(info.fn->id));
                        if (fn_sema != nullptr) {
                            const bool existed = [&]() {
                                for (size_t index = 0; index < instantiations->instanceCount();
                                     ++index) {
                                    const auto *candidate = instantiations->at(index);
                                    if (candidate != nullptr &&
                                        candidate->module == info.fn_module &&
                                        candidate->decl == info.fn->id &&
                                        candidate->args == info.fn_args)
                                        return true;
                                }
                                return false;
                            }();
                            instance_index =
                                instantiations->bindCall(current_module_->key, frontend::ExprId{},
                                                         info.fn_module, info.fn->id, info.fn_args);
                            if (!existed) {
                                const auto *instance = instantiations->at(instance_index);
                                if (instance != nullptr)
                                    predeclareInstantiation(instance->module, *instance);
                            }
                            (void)fn_sema;
                        }
                    }
                }
                if (instance_index != ~size_t{0}) {
                    const auto *instance = instantiations->at(instance_index);
                    for (const auto &function : functions_) {
                        if (function.instance != nullptr && function.module != nullptr &&
                            function.module->key == info.fn_module &&
                            function.instance->decl == info.fn->id && instance != nullptr &&
                            function.instance->args == instance->args) {
                            slot_sym = function.sym_id;
                            break;
                        }
                    }
                }
            }
            if (slot_sym == symbols::kInvalidSym) {
                if (const auto *fn_index = function_index_by_key_.get(key))
                    slot_sym = functions_[*fn_index].sym_id;
            }
            if (slot_sym != symbols::kInvalidSym)
                added.slots.push(slot_sym);
        }
        vtable = &added;
    }

    hir::HirMakeDyn make;
    make.value       = value;
    make.source_type = lowerType(concrete_sema);
    make.dyn_type    = target_hir;
    make.vtable_name = vtable_id;
    return addExpr(std::move(make));
}

hir::HirExprId HirLowerModern::lowerCoerceToOpaque(sema::modern::TypeId target,
                                                   frontend::ExprId expression,
                                                   hir::HirExprId value) {
    const types::TypeId target_hir = lowerType(target);
    if (value == hir::kInvalidHirExpr || !expression || current_module_ == nullptr ||
        current_types_ == nullptr || types_.kindOf(target_hir) != types::TypeKind::OpaqueTagged)
        return value;

    const auto *source_id = current_types_->opaqueSourceTypes.get(expression.value);
    if (source_id == nullptr)
        return value;
    const sema::modern::TypeId source_sema =
        current_instantiation_ != nullptr && current_instance_ != nullptr
            ? current_instantiation_->substituteType(*source_id, current_instance_->args)
            : *source_id;
    const types::TypeId source_type = lowerType(source_sema);
    if (source_type == types::kErrorType || source_type == types::kInvalidType)
        return value;

    hir::HirMakeOpaque make;
    make.value       = value;
    make.source_type = source_type;
    make.opaque_type = target_hir;
    make.type_id     = stableConcreteTypeId(source_type);
    return addExpr(std::move(make));
}

hir::HirExprId HirLowerModern::lowerSliceRange(const frontend::Expression &expr,
                                               const types::TypeId type) {
    if (expr.operands.size() < 3U)
        return hir::kInvalidHirExpr;
    const auto object = lowerExpr(expr.operands[0]);
    const auto lo     = lowerExpr(expr.operands[1]);
    const auto hi     = lowerExpr(expr.operands[2]);
    if (object == hir::kInvalidHirExpr || lo == hir::kInvalidHirExpr ||
        hi == hir::kInvalidHirExpr) {
        return hir::kInvalidHirExpr;
    }

    const auto object_type = typeOfExpr(expr.operands[0]);
    const auto *optional   = std::get_if<types::TypeOptional>(&types_.lookup(type));
    const auto slice_type  = optional != nullptr ? optional->inner : type;
    const auto bound_type  = typeOfExpr(expr.operands[1]);
    hir::HirMakeSlice slice;
    slice.object                           = object;
    slice.lo                               = lo;
    slice.hi                               = hi;
    slice.type                             = slice_type;
    slice.object_type                      = object_type;
    slice.bound_type                       = bound_type;
    slice.is_array                         = types_.kindOf(object_type) == types::TypeKind::Array;
    const sema::modern::TypeId sema_object = semaTypeOfExpr(expr.operands[0]);
    const auto *sema_optional =
        sema_.typeTable().optional(sema_.typeTable().stripQualifiers(sema_object));
    slice.is_pointer =
        types_.kindOf(object_type) == types::TypeKind::Ptr ||
        (sema_optional != nullptr && sema_.typeTable().pointer(sema_.typeTable().stripQualifiers(
                                         sema_optional->inner)) != nullptr);
    slice.checked = false;
    if (optional == nullptr || expr.is_raw)
        return addExpr(std::move(slice));

    // Evaluate the object and both bounds once, then branch on the dynamic checks.
    const auto object_slot = next_slot_++;
    const auto lo_slot     = next_slot_++;
    const auto hi_slot     = next_slot_++;
    const auto result_slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(object_slot, object_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(object_slot, object));
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(
        lo_slot,
        types_.kindOf(bound_type) == types::TypeKind::Int ? bound_type : types::kErrorType));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(lo_slot, lo));
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(
        hi_slot,
        types_.kindOf(bound_type) == types::TypeKind::Int ? bound_type : types::kErrorType));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(hi_slot, hi));
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(result_slot, type));

    const auto bound_load = emitSlotLoad(lo_slot, bound_type);
    const auto hi_load    = emitSlotLoad(hi_slot, bound_type);
    hir::HirLiteral zero;
    zero.type          = bound_type;
    zero.i             = 0;
    hir::HirExprId len = hir::kInvalidHirExpr;
    const auto *array  = std::get_if<types::TypeArray>(&types_.lookup(object_type));
    if (array != nullptr) {
        hir::HirLiteral len_lit;
        len_lit.type = bound_type;
        len_lit.i    = static_cast<int64_t>(array->count);
        len          = addExpr(std::move(len_lit));
    } else {
        const auto loaded = emitSlotLoad(object_slot, object_type);
        auto len_i64 =
            addExpr(hir::HirField{loaded, 1U, types_.internInt(types::IntWidth::I64), object_type});
        const auto len64 = types_.internInt(types::IntWidth::I64);
        if (types_.kindOf(bound_type) != types::TypeKind::Int) {
            len = hir::kInvalidHirExpr;
        } else if (bound_type != len64) {
            len = addExpr(hir::HirCast{len_i64, len64, bound_type});
        } else {
            len = len_i64;
        }
    }
    if (len == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    hir::HirBinary lo_ge_zero;
    lo_ge_zero.lhs           = bound_load;
    lo_ge_zero.rhs           = addExpr(std::move(zero));
    lo_ge_zero.op            = hir::HirBinaryOp::Ge;
    lo_ge_zero.type          = types::kBoolType;
    lo_ge_zero.operand_type  = bound_type;
    const auto lo_ge_zero_id = addExpr(std::move(lo_ge_zero));

    hir::HirBinary hi_le_len;
    hi_le_len.lhs           = hi_load;
    hi_le_len.rhs           = len;
    hi_le_len.op            = hir::HirBinaryOp::Le;
    hi_le_len.type          = types::kBoolType;
    hi_le_len.operand_type  = bound_type;
    const auto hi_le_len_id = addExpr(std::move(hi_le_len));

    hir::HirBinary lo_le_hi;
    lo_le_hi.lhs           = bound_load;
    lo_le_hi.rhs           = hi_load;
    lo_le_hi.op            = hir::HirBinaryOp::Le;
    lo_le_hi.type          = types::kBoolType;
    lo_le_hi.operand_type  = bound_type;
    const auto lo_le_hi_id = addExpr(std::move(lo_le_hi));

    hir::HirBinary first_and;
    first_and.lhs           = lo_ge_zero_id;
    first_and.rhs           = hi_le_len_id;
    first_and.op            = hir::HirBinaryOp::And;
    first_and.type          = types::kBoolType;
    first_and.operand_type  = types::kBoolType;
    const auto first_and_id = addExpr(std::move(first_and));

    hir::HirBinary all_ok;
    all_ok.lhs          = first_and_id;
    all_ok.rhs          = lo_le_hi_id;
    all_ok.op           = hir::HirBinaryOp::And;
    all_ok.type         = types::kBoolType;
    all_ok.operand_type = types::kBoolType;

    const auto some_block  = newBlock();
    const auto none_block  = newBlock();
    const auto merge_block = newBlock();
    hir::HirBranch branch;
    branch.cond       = addExpr(std::move(all_ok));
    branch.then_block = static_cast<hir::HirDeclId>(some_block);
    branch.else_block = static_cast<hir::HirDeclId>(none_block);
    setTerminator(addExpr(std::move(branch)));

    setCurrentBlock(some_block);
    current_fn_->blocks[some_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    {
        hir::HirMakeSlice ok_slice;
        ok_slice.object      = emitSlotLoad(object_slot, object_type);
        ok_slice.lo          = emitSlotLoad(lo_slot, bound_type);
        ok_slice.hi          = emitSlotLoad(hi_slot, bound_type);
        ok_slice.type        = slice_type;
        ok_slice.object_type = object_type;
        ok_slice.bound_type  = bound_type;
        ok_slice.is_array    = slice.is_array;
        ok_slice.is_pointer  = slice.is_pointer;
        ok_slice.checked     = false;
        hir::HirMakeSome some;
        some.type  = type;
        some.value = addExpr(std::move(ok_slice));
        current_fn_->blocks[some_block].insts.push(
            emitSlotStore(result_slot, addExpr(std::move(some))));
        emitJump(merge_block);
    }

    setCurrentBlock(none_block);
    current_fn_->blocks[none_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    {
        hir::HirMakeNone none;
        none.type = type;
        current_fn_->blocks[none_block].insts.push(
            emitSlotStore(result_slot, addExpr(std::move(none))));
        emitJump(merge_block);
    }

    setCurrentBlock(merge_block);
    current_fn_->blocks[merge_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    return emitSlotLoad(result_slot, type);
}

memory::Optional<int64_t> HirLowerModern::enumVariantValue(frontend::ExprId operand,
                                                           std::string_view variant) {
    if (!operand || current_module_ == nullptr || current_module_->frontend == nullptr ||
        operand.value > current_module_->frontend->expressions().size()) {
        return {};
    }
    const auto &op = current_module_->frontend->expressions()[operand.value - 1U];
    if (op.kind != frontend::ExprKind::Name)
        return {};
    const auto *resolved = findResolvedExpr(operand);
    if (resolved == nullptr || !resolved->declaration)
        return {};
    const auto *decl = findDecl(*current_module_, resolved->declaration);
    if (decl == nullptr || decl->kind != frontend::DeclKind::Enum)
        return {};
    // Enum variants are constants, but generic enum templates lower through a
    // concrete instance. Resolve the discriminant from the expression's actual
    // sema type (`Status<i32>`) instead of the template declaration.
    const auto sema_enum_type = sema_.typeTable().stripQualifiers(semaTypeOfExpr(operand));
    const auto *et            = sema_.typeTable().enum_type(sema_enum_type);
    if (et == nullptr)
        return {};
    for (size_t i = 0; i < et->variant_names.size(); ++i) {
        if (et->variant_names[i] == variant)
            return et->discriminants[i];
    }
    return {};
}

hir::HirExprId HirLowerModern::lowerField(const frontend::Expression &expr,
                                          const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    if (current_types_ != nullptr) {
        if (const auto *base = current_types_->traitQualifiedReceiverBase.get(expr.id.value))
            return lowerExpr(frontend::ExprId{*base});
    }
    // `console.println` where `console` is an import alias: the field expression resolves
    // to the imported symbol, so emit the same HirVar a plain name would produce and never
    // lower the alias base (which would fail to resolve 'console').
    if (const auto *resolved = findResolvedExpr(expr.id);
        resolved != nullptr && resolved->kind == session::ResolutionKind::Import) {
        const frontend::Declaration *decl = resolvedFunctionDecl(*resolved);
        if (decl != nullptr) {
            hir::HirVar var;
            var.name    = interner_.intern(decl->name);
            var.version = 0;
            return addExpr(std::move(var));
        }
        if (resolved->foreignFunction != nullptr) {
            hir::HirVar var;
            var.name    = interner_.intern(resolved->foreignFunction->linkageName);
            var.version = 0;
            return addExpr(std::move(var));
        }
        return hir::kInvalidHirExpr;
    }
    // `std.io.console.println(...)`, `std.counter.Counter`-style chains, and
    // other multi-segment module paths have intermediate Field nodes that
    // resolve as ModuleAlias. Those nodes carry no value; the final member is
    // the Import binding above, so intervening aliases must not lower.
    if (const auto *resolved = findResolvedExpr(expr.id);
        resolved != nullptr && resolved->kind == session::ResolutionKind::ModuleAlias) {
        const frontend::Expression *chain = &expr;
        while (chain->kind == frontend::ExprKind::Field && !chain->operands.empty()) {
            const auto &base = current_module_->frontend->expressions()[chain->operands[0].value - 1U];
            if (const auto *base_resolved = findResolvedExpr(chain->operands[0]);
                base_resolved != nullptr &&
                base_resolved->kind == session::ResolutionKind::Import) {
                const frontend::Declaration *decl = resolvedFunctionDecl(*base_resolved);
                if (decl != nullptr) {
                    hir::HirVar var;
                    var.name    = interner_.intern(decl->name);
                    var.version = 0;
                    return addExpr(std::move(var));
                }
                return hir::kInvalidHirExpr;
            }
            chain = &base;
        }
    }
    // `Color.Green` resolves to an enum variant constant, not a struct field read.
    if (const auto variant = enumVariantValue(expr.operands[0], expr.text))
        return addExpr(hir::HirEnumValue{*variant, type});
    auto object      = lowerExpr(expr.operands[0]);
    auto object_type = typeOfExpr(expr.operands[0]);
    if (object == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    // Resolve the sema struct type to find the field index by name
    auto sema_type = sema_.typeTable().stripQualifiers(semaTypeOfExpr(expr.operands[0]));
    // `self.field` / `p.field` auto-derefs an implicit receiver or borrow
    // parameter in sema; mirror it here so the field access still lowers to a
    // loaded value.
    if (const auto *sem_ptr = sema_.typeTable().pointer(sema_type)) {
        const auto struct_type = lowerType(sema_.typeTable().stripQualifiers(sem_ptr->pointee));
        object      = addExpr(hir::HirUnary{hir::HirUnaryOp::Deref, object, struct_type});
        object_type = struct_type;
        sema_type   = sema_.typeTable().stripQualifiers(sem_ptr->pointee);
    }
    const int idx = sema_.typeTable().fieldIndex(sema_type, expr.text);
    if (idx >= 0)
        return addExpr(hir::HirField{object, static_cast<uint32_t>(idx), type, object_type});
    if (sema_.typeTable().kindOf(sema_type) == sema::modern::TypeKind::Pack) {
        const auto *pack = sema_.typeTable().pack(sema_type);
        if (pack != nullptr) {
            for (size_t index = 0; index < pack->names.size(); ++index) {
                if (pack->names[index] != expr.text)
                    continue;
                return addExpr(
                    hir::HirField{object, static_cast<uint32_t>(index), type, object_type});
            }
        }
    }
    if (current_module_ != nullptr &&
        sema_.typeTable().kindOf(sema_type) == sema::modern::TypeKind::GenericParam) {
        const auto *module_sema = sema_.findModuleSema(current_module_->key);
        if (module_sema != nullptr) {
            for (const TypeId bound : module_sema->boundsForGenericParam(sema_type)) {
                if (!module_sema->isInterfaceType(bound))
                    continue;
                const auto *trait_ty = sema_.typeTable().trait(module_sema->resolve(bound));
                if (trait_ty == nullptr)
                    continue;
                const auto *iface =
                    module_sema->findDeclNamed(trait_ty->name, frontend::DeclKind::Interface);
                if (iface == nullptr)
                    continue;
                for (size_t field = 0; field < iface->parameters.size(); ++field) {
                    if (iface->parameters[field].name != expr.text)
                        continue;
                    return addExpr(
                        hir::HirField{object, static_cast<uint32_t>(field), type, object_type});
                }
            }
        }
    }
    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerArrow(const frontend::Expression &expr,
                                          const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    const auto ptr = lowerExpr(expr.operands[0]);
    if (ptr == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    // Deref the pointer first
    auto sema_ptr_type = sema_.typeTable().stripQualifiers(semaTypeOfExpr(expr.operands[0]));
    // Match sema: `?*T` behaves as `*T` here because None is represented as nullptr.
    if (const auto *opt = sema_.typeTable().optional(sema_ptr_type))
        sema_ptr_type = sema_.typeTable().stripQualifiers(opt->inner);
    const auto *pt = sema_.typeTable().pointer(sema_ptr_type);
    if (pt == nullptr)
        return hir::kInvalidHirExpr;
    const auto sema_struct = sema_.typeTable().stripQualifiers(pt->pointee);
    const auto struct_type = lowerType(sema_struct);
    const auto deref       = addExpr(hir::HirUnary{hir::HirUnaryOp::Deref, ptr, struct_type});
    const int idx          = sema_.typeTable().fieldIndex(sema_struct, expr.text);
    if (idx < 0)
        return hir::kInvalidHirExpr;
    return addExpr(hir::HirField{deref, static_cast<uint32_t>(idx), type, struct_type});
}

hir::HirExprId HirLowerModern::lowerStructLiteral(const frontend::Expression &expr,
                                                  const types::TypeId type) {
    if (types_.kindOf(type) == types::TypeKind::Union) {
        if (expr.operands.size() != 1U)
            return hir::kInvalidHirExpr;
        const auto value = lowerExpr(expr.operands[0]);
        if (value == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        const auto from = typeOfExpr(expr.operands[0]);
        hir::HirUnionCast cast;
        cast.value        = value;
        cast.from         = from;
        cast.to           = type;
        cast.member_index = taggedMemberIndex(type, from);
        cast.checked      = false;
        return addExpr(std::move(cast));
    }
    hir::HirStructLiteral lit(arena_);
    lit.type = type;
    const size_t field_count =
        types_.kindOf(type) == types::TypeKind::Struct ? types_.fieldCount(type) : 0U;
    // Values are emitted in declaration order, not in the order the literal was written.
    std::vector<hir::HirExprId> ordered(field_count == 0U ? expr.operands.size() : field_count,
                                        hir::kInvalidHirExpr);
    for (size_t i = 0; i < expr.operands.size(); ++i) {
        auto value = lowerExpr(expr.operands[i]);
        if (value == hir::kInvalidHirExpr)
            continue;
        size_t slot_index = i;
        if (field_count != 0U && i < expr.field_names.size()) {
            slot_index = types_.fieldIndex(type, expr.field_names[i]);
            if (slot_index >= field_count)
                continue;
        }
        if (field_count != 0U) {
            // A bare `T` value assigned to a `?T` field must be wrapped in Some.
            const auto field_type = types_.getField(type, slot_index).type;
            const auto value_type = typeOfExpr(expr.operands[i]);
            if (types_.kindOf(field_type) == types::TypeKind::Optional &&
                types_.kindOf(value_type) != types::TypeKind::Optional) {
                value = lowerCoerceToOptional(field_type, value);
            }
            value = lowerCoerceToTarget(field_type, expr.operands[i], value);
        }
        if (slot_index < ordered.size())
            ordered[slot_index] = value;
    }
    // Fill slots left empty by a `_` placeholder or an omitted field with the
    // struct declaration's default; slots without a default stay zero-initialized.
    if (field_count != 0U) {
        for (size_t slot_index = 0; slot_index < field_count; ++slot_index) {
            if (ordered[slot_index] != hir::kInvalidHirExpr)
                continue;
            const auto default_id = lowerFieldDefault(expr.text, slot_index);
            if (!default_id)
                continue;
            const auto default_value = lowerExpr(default_id);
            if (default_value == hir::kInvalidHirExpr)
                continue;
            const auto field_type = types_.getField(type, slot_index).type;
            ordered[slot_index] =
                types_.kindOf(field_type) == types::TypeKind::Optional &&
                        types_.kindOf(typeOfExpr(default_id)) != types::TypeKind::Optional
                    ? lowerCoerceToOptional(field_type, default_value)
                    : lowerCoerceToTarget(field_type, default_id, default_value);
        }
    }
    // Keep every slot (missing ones are zero at codegen); the array is index-aligned.
    for (const auto value : ordered)
        lit.values.push(value);
    return addExpr(std::move(lit));
}

hir::HirExprId HirLowerModern::lowerPackLiteral(const frontend::Expression &expr,
                                                const types::TypeId type) {
    hir::HirStructLiteral lit(arena_);
    lit.type = type;
    for (const auto operand : expr.operands) {
        const auto value = lowerExpr(operand);
        if (value == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        lit.values.push(value);
    }
    return addExpr(std::move(lit));
}

hir::HirExprId HirLowerModern::lowerArrayLiteral(const frontend::Expression &expr,
                                                 const types::TypeId type) {
    hir::HirArrayLiteral lit(arena_);
    lit.type                 = type;
    const auto *target_array = std::get_if<types::TypeArray>(&types_.lookup(type));
    for (size_t index = 0; index < expr.operands.size(); ++index) {
        const frontend::ExprId operand = expr.operands[index];
        auto value                     = lowerExpr(operand);
        if (value == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        if (target_array != nullptr)
            value = lowerCoerceToTarget(target_array->elem, operand, value);
        lit.elements.push(value);
    }
    return addExpr(std::move(lit));
}

frontend::ExprId HirLowerModern::lowerFieldDefault(std::string_view struct_name,
                                                   size_t field_index) const noexcept {
    if (current_module_ == nullptr || current_module_->frontend == nullptr)
        return {};
    const auto &decls = current_module_->frontend->declarations();
    for (const auto &decl : decls) {
        if (decl.kind != frontend::DeclKind::Struct || decl.name != struct_name)
            continue;
        if (field_index < decl.parameters.size())
            return decl.parameters[field_index].defaultValue;
        break;
    }
    return {};
}

hir::HirExprId HirLowerModern::lowerCoerceToOptional(types::TypeId target, hir::HirExprId value) {
    if (value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    // This is a simple wrapper: wrap any T value into ?T (Some)
    hir::HirMakeSome some;
    some.type  = target;
    some.value = value;
    return addExpr(std::move(some));
}

hir::HirExprId HirLowerModern::lowerMakeNone(types::TypeId target) {
    hir::HirMakeNone make_none;
    make_none.type = target;
    return addExpr(std::move(make_none));
}

hir::HirExprId HirLowerModern::lowerCoerceToOptionalDepth(types::TypeId target,
                                                          sema::modern::TypeId target_sema,
                                                          sema::modern::TypeId source_sema,
                                                          hir::HirExprId value) {
    if (value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    if (target == types::kInvalidType || target == types::kErrorType ||
        target_sema == sema::modern::kInvalidTypeId || source_sema == sema::modern::kInvalidTypeId)
        return value;

    // Apply the missing optional layers one at a time. Each layer is a `Some`
    // around the previous value, so codegen sees the exact LLVM aggregate or
    // pointer representation of each nested optional type.
    auto current      = value;
    auto current_sema = source_sema;
    const auto *module_sema =
        current_module_ != nullptr ? sema_.findModuleSema(current_module_->key) : nullptr;
    while (sema_.typeTable().kindOf(sema_.typeTable().stripQualifiers(target_sema)) ==
               sema::modern::TypeKind::Optional &&
           module_sema != nullptr && !module_sema->sameType(target_sema, current_sema) &&
           module_sema->coercesTo(target_sema, current_sema)) {
        const auto *target_opt =
            sema_.typeTable().optional(sema_.typeTable().stripQualifiers(target_sema));
        if (target_opt == nullptr)
            return value;

        const bool source_is_optional =
            sema_.typeTable().kindOf(sema_.typeTable().stripQualifiers(current_sema)) ==
            sema::modern::TypeKind::Optional;
        // The first wrap from a bare `T` creates the nearest optional layer.
        // Once the value is already optional, the remaining wraps apply the
        // full outer optional type.
        const auto layer_sema =
            (source_is_optional ||
             sema_.typeTable().kindOf(target_opt->inner) != sema::modern::TypeKind::Optional)
                ? target_sema
                : target_opt->inner;
        const auto layer_hir = lowerType(layer_sema);
        if (layer_hir == types::kErrorType || layer_hir == types::kInvalidType)
            return value;

        hir::HirMakeSome some;
        some.type    = layer_hir;
        some.value   = current;
        current      = addExpr(std::move(some));
        current_sema = layer_sema;
        if (source_is_optional)
            target_sema = target_opt->inner;
    }
    return current;
}

bool HirLowerModern::lowerStatement(frontend::StmtId id, hir::HirExprId &last_value) {
    if (!id || current_module_ == nullptr ||
        id.value > current_module_->frontend->statements().size())
        return true;

    const auto &statement = current_module_->frontend->statements()[id.value - 1U];
    switch (statement.kind) {
    case frontend::StmtKind::Expression:
        if (statement.expression &&
            statement.expression.value <= current_module_->frontend->expressions().size()) {
            last_value = lowerExpr(statement.expression);
            if (last_value == hir::kInvalidHirExpr &&
                typeOfExpr(statement.expression) != types::kVoidType &&
                typeOfExpr(statement.expression) != types::kErrorType && !diags_.hasErrors()) {
                diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                              "expression statement could not be lowered", memory::Span{});
                return false;
            }
            if (last_value != hir::kInvalidHirExpr) {
                if (defer_body_sink_ != nullptr)
                    defer_body_sink_->push(last_value);
                else
                    current_fn_->blocks[current_block_].insts.push(last_value);
            }
        }
        return true;
    case frontend::StmtKind::Declaration:
        // Local states have already been predeclared and are not executed.
        return true;
    case frontend::StmtKind::Binding: {
        // A for-in element binding is materialized by the loop lowering: it
        // only stores the per-iteration `value()` result, so allocating the
        // same slot again here would clobber that store in codegen.
        if (statement.id == current_for_in_binding_stmt_ ||
            (current_for_in_binding_local_ &&
             statement.binding.id == current_for_in_binding_local_)) {
            last_value = hir::kInvalidHirExpr;
            return true;
        }
        const auto slot   = localSlot(statement.binding.id);
        const auto type   = typeOfLocal(statement.binding.id);
        const auto alloca = emitSlotAlloca(slot, type);
        if (defer_body_sink_ != nullptr)
            defer_body_sink_->push(alloca);
        else
            current_fn_->blocks[current_block_].insts.push(alloca);
        if (statement.binding.initializer) {
            auto init = lowerExpr(statement.binding.initializer);
            // Sema now accepts `T -> ?T`, `?T -> ??T`, and deeper optional
            // coercions. The HIR target is canonical by construction only when
            // the coerced source has the same flattened payload layout, so we
            // wrap according to the sema source type instead of forcing a
            // `?T` value into a `??T` slot.
            if (init != hir::kInvalidHirExpr && types_.kindOf(type) == types::TypeKind::Optional) {
                const auto binding_sema_type = semaTypeOfLocal(statement.binding.id);
                const auto init_sema_type    = semaTypeOfExpr(statement.binding.initializer);
                if (binding_sema_type != sema::modern::kInvalidTypeId &&
                    init_sema_type != sema::modern::kInvalidTypeId)
                    init =
                        lowerCoerceToOptionalDepth(type, binding_sema_type, init_sema_type, init);
            }
            init = lowerCoerceToTarget(type, statement.binding.initializer, init);
            const sema::modern::TypeId binding_sema = semaTypeOfLocal(statement.binding.id);
            if (binding_sema != sema::modern::kInvalidTypeId &&
                sema_.typeTable().kindOf(binding_sema) == sema::modern::TypeKind::Dyn)
                init = lowerCoerceToDyn(binding_sema, statement.binding.initializer, init,
                                        binding_sema);
            else if (binding_sema != sema::modern::kInvalidTypeId)
                init = lowerCoerceToOpaque(binding_sema, statement.binding.initializer, init);
            if (init != hir::kInvalidHirExpr) {
                const auto store = emitSlotStore(slot, init);
                if (defer_body_sink_ != nullptr)
                    defer_body_sink_->push(store);
                else
                    current_fn_->blocks[current_block_].insts.push(store);
            }
        }
        last_value = hir::kInvalidHirExpr;
        return true;
    }
    case frontend::StmtKind::Defer:
        // In ordinary lexical blocks, defers are flushed at end-of-block after
        // all direct bindings have emitted their slots. Inside a deferred body
        // (`defer { defer x(); }`) they lower immediately into that body in
        // source order.
        if (defer_body_sink_ != nullptr)
            return lowerDeferBody(id);
        if (pending_defers_.empty())
            pending_defers_.emplace_back();
        pending_defers_.back().push_back(id);
        last_value = hir::kInvalidHirExpr;
        return true;
    case frontend::StmtKind::Return: {
        emitCleanupFrom(0);
        hir::HirRet ret;
        if (statement.expression) {
            auto value = lowerExpr(statement.expression);
            // Coerce T → ?T when the current block's return statement carries an
            // expression whose sema type is not optional.  The implicit-return
            // path below performs the same coercion when the function returns
            // the block value.
            if (value != hir::kInvalidHirExpr &&
                types_.kindOf(current_fn_->return_type) == types::TypeKind::Optional) {
                const auto val_sema_type = semaTypeOfExpr(statement.expression);
                if (current_fn_return_sema_type_ != sema::modern::kInvalidTypeId &&
                    val_sema_type != sema::modern::kInvalidTypeId)
                    value = lowerCoerceToOptionalDepth(current_fn_->return_type,
                                                       current_fn_return_sema_type_, val_sema_type,
                                                       value);
                else if (sema_.typeTable().kindOf(val_sema_type) != TypeKind::Optional)
                    value = lowerCoerceToOptional(current_fn_->return_type, value);
            }
            value = lowerCoerceToTarget(current_fn_->return_type, statement.expression, value);
            if (current_fn_return_sema_type_ != sema::modern::kInvalidTypeId)
                value = lowerCoerceToDyn(current_fn_return_sema_type_, statement.expression, value,
                                         current_fn_return_sema_type_);
            if (current_fn_return_sema_type_ != sema::modern::kInvalidTypeId)
                value =
                    lowerCoerceToOpaque(current_fn_return_sema_type_, statement.expression, value);
            ret.value = value;
        }
        if (!statement.expression && current_main_void_) {
            // The bare `return;` in a void main returns success from the C
            // entry point, whose HIR signature is i32 for the linker.
            hir::HirLiteral zero;
            zero.type = types_.internInt(types::IntWidth::I32);
            zero.i    = 0;
            ret.value = addExpr(std::move(zero));
        }
        setTerminator(addExpr(std::move(ret)));
        return true;
    }
    case frontend::StmtKind::Break:
        if (loop_stack_.empty()) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "break used outside of a loop", {});
            return false;
        }
        {
            const LoopTarget *target = &loop_stack_.back();
            if (!statement.label.empty()) {
                target = nullptr;
                for (auto it = loop_stack_.rbegin(); it != loop_stack_.rend(); ++it) {
                    if (it->label == statement.label) {
                        target = &*it;
                        break;
                    }
                }
                if (target == nullptr) {
                    diags_.report(
                        diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                        "break label '" + statement.label + "' does not name an active loop", {});
                    return false;
                }
            }
            emitCleanupFrom(target->cleanup_depth);
            emitJump(target->break_block);
        }
        setCurrentBlock(newBlock());
        current_fn_->blocks[current_block_].insts = memory::DynArray<hir::HirExprId>(arena_);
        return true;
    case frontend::StmtKind::Continue:
        if (loop_stack_.empty()) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "continue used outside of a loop", {});
            return false;
        }
        {
            const LoopTarget *target = &loop_stack_.back();
            if (!statement.label.empty()) {
                target = nullptr;
                for (auto it = loop_stack_.rbegin(); it != loop_stack_.rend(); ++it) {
                    if (it->label == statement.label) {
                        target = &*it;
                        break;
                    }
                }
                if (target == nullptr) {
                    diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                                  "continue label '" + statement.label +
                                      "' does not name an active loop",
                                  {});
                    return false;
                }
            }
            emitCleanupFrom(target->cleanup_depth);
            emitJump(target->continue_block);
        }
        setCurrentBlock(newBlock());
        current_fn_->blocks[current_block_].insts = memory::DynArray<hir::HirExprId>(arena_);
        return true;
    case frontend::StmtKind::Jump: {
        if (!current_fn_is_state_) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "jump is only allowed inside a state function", {});
            return false;
        }
        const frontend::Declaration *target = nullptr;
        if (current_module_ != nullptr && current_module_->frontend != nullptr) {
            for (const auto &decl : current_module_->frontend->declarations()) {
                if (decl.kind != frontend::DeclKind::Function || decl.name != statement.label ||
                    decl.functionKind != frontend::FunctionKind::State ||
                    decl.parentScope != info_decl_parent_scope_) {
                    continue;
                }
                target = &decl;
                break;
            }
        }
        if (target == nullptr) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "jump target must be a state function: '" + statement.label + "'", {});
            return false;
        }
        emitCleanupFrom(0);
        const auto target_key      = internFunctionKey(interner_, current_module_->key, target->id);
        const auto *function_index = function_index_by_key_.get(target_key);
        if (function_index == nullptr) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "state target was not predeclared: '" + statement.label + "'", {});
            return false;
        }

        const auto *module_sema = sema_.findModuleSema(current_module_->key);
        if (module_sema == nullptr ||
            module_sema->stateMachineIdOf(*target) != current_state_machine_id_) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "state transition target is in a different state machine", {});
            return false;
        }

        hir::HirStateTailCall tail(arena_);
        tail.call.callee          = hir::kInvalidHirExpr;
        const auto target_fn_type = module_sema->typeOfDecl(target->id);
        const auto *target_fn     = sema_.typeTable().function(target_fn_type);
        if (target_fn == nullptr) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "state target has no function type: '" + statement.label + "'", {});
            return false;
        }
        const bool target_is_slice =
            target_fn->params.size() > 0 && target->parameters.back().isVariadicSlice;
        const size_t slice_index =
            target_is_slice ? target_fn->params.size() - 1U : target_fn->params.size();
        const bool explicit_slice_arg =
            target_is_slice && statement.arguments.size() == slice_index + 1U &&
            !statement.arguments.empty() &&
            (types_.kindOf(typeOfExpr(statement.arguments.back())) == types::TypeKind::Slice ||
             types_.kindOf(typeOfExpr(statement.arguments.back())) == types::TypeKind::Array);
        const bool auto_collect_tail = target_is_slice && !explicit_slice_arg;

        // Sema already rejected arity/type mismatches before HIR lowering.
        // Lower only the arguments that exist; a variadic slice tail is packed
        // below, and an explicit slice argument is kept as-is.
        size_t lowered_fixed = 0;
        for (; lowered_fixed < std::min(statement.arguments.size(), slice_index); ++lowered_fixed) {
            const auto &arg_expr =
                current_module_->frontend
                    ->expressions()[statement.arguments[lowered_fixed].value - 1U];
            const bool annotated =
                arg_expr.kind == frontend::ExprKind::OwnershipCoerce && !arg_expr.operands.empty();
            const frontend::ExprId inner_id =
                annotated ? arg_expr.operands[0] : statement.arguments[lowered_fixed];
            auto argument = lowerExpr(inner_id);
            if (annotated) {
                auto address = lowerLValueAddr(inner_id);
                if (address == hir::kInvalidHirExpr) {
                    const auto inner_type = typeOfExpr(inner_id);
                    const auto slot       = next_slot_++;
                    current_fn_->blocks[current_block_].insts.push(
                        emitSlotAlloca(slot, inner_type));
                    current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, argument));
                    address = addExpr(hir::HirSlotAddr{slot, inner_type});
                }
                argument = address;
            }
            if (argument == hir::kInvalidHirExpr)
                return false;
            argument = lowerCoerceToTarget(lowerType(target_fn->params[lowered_fixed]),
                                           statement.arguments[lowered_fixed], argument);
            argument = lowerCoerceToOpaque(target_fn->params[lowered_fixed],
                                           statement.arguments[lowered_fixed], argument);
            tail.call.argument_types.push(lowerType(target_fn->params[lowered_fixed]));
            tail.call.args.push(argument);
        }
        if (target != nullptr && current_module_ != nullptr) {
            const comptime::InstantiationInstance *target_instance = nullptr;
            if (const auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
                const auto *func_index2 = function_index_by_key_.get(target_key);
                if (func_index2 != nullptr) {
                    const auto &function = functions_[*func_index2];
                    if (function.instance != nullptr && function.instance->decl == target->id) {
                        for (size_t index = 0; index < instantiations->instanceCount(); ++index) {
                            const auto *instance = instantiations->at(index);
                            if (instance->decl == target->id &&
                                instance->args == function.instance->args) {
                                target_instance = instance;
                                break;
                            }
                        }
                    }
                }
            }
            for (size_t index = lowered_fixed;
                 index < target->parameters.size() && index < slice_index; ++index) {
                if (!target->parameters[index].defaultValue)
                    continue;
                auto value = lowerDefaultWithTarget(*current_module_, target_instance,
                                                    target->parameters[index].defaultValue,
                                                    target_fn->params[index]);
                if (value == hir::kInvalidHirExpr)
                    return false;
                tail.call.args.push(value);
                tail.call.argument_types.push(lowerType(target_fn->params[index]));
            }
        }
        if (auto_collect_tail && statement.arguments.size() > slice_index) {
            std::vector<frontend::ExprId> tail_exprs(statement.arguments.begin() +
                                                         static_cast<ptrdiff_t>(slice_index),
                                                     statement.arguments.end());
            auto slice = lowerVariadicSliceTail(target_fn->params[slice_index], tail_exprs);
            if (slice == hir::kInvalidHirExpr)
                return false;
            tail.call.argument_types.push(lowerType(target_fn->params[slice_index]));
            tail.call.args.push(slice);
        } else if (explicit_slice_arg ||
                   (target_is_slice && statement.arguments.size() == slice_index)) {
            // The slice parameter is vacuous (`f()`) or passed as a single
            // explicit slice value.
            const auto slice_type = lowerType(target_fn->params[slice_index]);
            if (!explicit_slice_arg) {
                auto slice = lowerVariadicSliceTail(target_fn->params[slice_index], {});
                if (slice == hir::kInvalidHirExpr)
                    return false;
                tail.call.argument_types.push(lowerType(target_fn->params[slice_index]));
                tail.call.args.push(slice);
            } else {
                auto argument = lowerExpr(statement.arguments.back());
                if (argument == hir::kInvalidHirExpr)
                    return false;
                argument = lowerCoerceToTarget(slice_type, statement.arguments.back(), argument);
                argument = lowerCoerceToOpaque(target_fn->params[slice_index],
                                               statement.arguments.back(), argument);
                tail.call.argument_types.push(lowerType(target_fn->params[slice_index]));
                tail.call.args.push(argument);
            }
        }
        tail.call.resolved_fn = functions_[*function_index].sym_id;
        tail.call.musttail    = true;
        tail.call.usesTailCC  = true;
        setTerminator(addExpr(std::move(tail)));
        last_value = hir::kInvalidHirExpr;
        return true;
    }
    case frontend::StmtKind::Error:
        return false;
    }
    return true;
}

hir::HirExprId HirLowerModern::addExpr(hir::HirExpr expr) {
    return hir_.addExpr(std::move(expr));
}

hir::HirExprId HirLowerModern::emitSlotAlloca(const hir::HirSlotId slot, const types::TypeId type) {
    return addExpr(hir::HirSlotAlloca{slot, type});
}

hir::HirExprId HirLowerModern::emitSlotStore(const hir::HirSlotId slot,
                                             const hir::HirExprId value) {
    return addExpr(hir::HirSlotStore{slot, value});
}

hir::HirExprId HirLowerModern::emitSlotLoad(const hir::HirSlotId slot, const types::TypeId type) {
    return addExpr(hir::HirSlotLoad{slot, type});
}

size_t HirLowerModern::newBlock() {
    const auto block = current_fn_->blocks.size();
    current_fn_->blocks.emplace(arena_);
    return block;
}

void HirLowerModern::setCurrentBlock(const size_t block) {
    current_block_ = block;
}

void HirLowerModern::setTerminator(const hir::HirExprId term) {
    current_fn_->blocks[current_block_].terminator = term;
}

void HirLowerModern::emitJump(const size_t target) {
    hir::HirJump jump;
    jump.target = static_cast<hir::HirDeclId>(target);
    setTerminator(addExpr(std::move(jump)));
}

} // namespace zith::sema::modern
