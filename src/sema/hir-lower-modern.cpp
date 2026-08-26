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
            // source name; everything else is qualified so overloads and
            // same-named functions in different modules get distinct symbols.
            std::string fn_name = decl.name;
            if (!decl.isExtern && decl.name != "main") {
                const auto name_space = moduleNamespace(module.key, snapshot_.cacheKey());
                std::string qualified;
                if (!name_space.empty())
                    qualified = name_space + ".";
                if (!decl.ownerName.empty())
                    qualified += decl.ownerName + ".";
                qualified += decl.name;
                qualified += frontend::functionSignature(*module.frontend, decl);
                fn_name = std::move(qualified);
            } else if (!decl.ownerName.empty()) {
                fn_name = decl.ownerName + "." + fn_name;
            }

            auto &hir_fn      = hir_.addFn(interner_.intern(fn_name));
            hir_fn.sym_id     = static_cast<symbols::SymId>(functions_.size() + next_sym_id_);
            hir_fn.decl_id    = static_cast<ast::DeclId>(decl.id.value);
            hir_fn.fnSpan     = memory::Span{0, decl.span.start, decl.span.end};
            hir_fn.isVariadic = decl.isVariadic;

            const auto fn_type = module_sema->typeOfDecl(decl.id);
            if (const auto *fn = sema_.typeTable().function(fn_type)) {
                hir_fn.return_type = lowerType(fn->result);
                for (size_t index = 0; index < decl.parameters.size(); ++index) {
                    const auto &parameter = decl.parameters[index];
                    const auto param_type = index < fn->params.size() ? lowerType(fn->params[index])
                                                                      : types::kErrorType;
                    hir_fn.params.push(param_type);
                    hir_fn.param_names.push(interner_.intern(parameter.name));
                }
            } else {
                hir_fn.return_type = types::kVoidType;
                for (const auto &parameter : decl.parameters) {
                    hir_fn.params.push(types::kErrorType);
                    hir_fn.param_names.push(interner_.intern(parameter.name));
                }
            }

            const auto key = internFunctionKey(interner_, module.key, decl.id);
            function_index_by_key_.insert(key, hir_.getFnCount() - 1U);
            functions_.push_back({key, module_ptr.get(), &decl, nullptr, nullptr, hir_fn.sym_id,
                                  hir_.getFnCount() - 1U});
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
    session::ModuleKey last_module_key;
    for (const auto &module_ptr : snapshot_.modules()) {
        if (module_ptr == nullptr)
            continue;
        if (module_ptr->key != last_module_key) {
            ensureModuleMarkers(*module_ptr);
            last_module_key = module_ptr->key;
        }
    }
    for (auto &function : functions_) {
        if (function.decl != nullptr && function.decl->body && !lowerFunctionBody(function))
            return false;
    }
    uint32_t blob_size  = 1;
    uint32_t blob_align = 1;
    for (auto &marker : hir_.markers().markers) {
        uint32_t marker_offset = 0;
        for (auto &param : marker.params) {
            const auto size  = lowerTypeSize(param.type);
            const auto align = lowerTypeAlign(param.type);
            if (size == 0 || align == 0)
                continue;
            marker_offset = alignUp(marker_offset, align);
            param.offset  = marker_offset;
            marker_offset += size;
            if (align > blob_align)
                blob_align = align;
        }
        marker.blob_offset = marker_offset;
        if (marker_offset > blob_size)
            blob_size = marker_offset;
    }
    hir_.setModuleMarkerLayout(blob_size, blob_align);
    return !diags_.hasErrors();
}

void HirLowerModern::ensureModuleMarkers(const session::ModuleArtifact &module) {
    if (module.key == currentMarkerModule_)
        return;
    currentMarkerModule_ = module.key;
    globalMarkerByName_.clear();
    markerSources_.clear();
    markerIdByStmt_.clear();
    markerIdByDecl_.clear();
    nextMarkerId_ = 0;
    if (module.frontend == nullptr)
        return;
    for (const auto &decl : module.frontend->declarations()) {
        if (decl.kind != frontend::DeclKind::Marker || decl.name.empty())
            continue;
        const auto marker = addMarkerMetadata(module, decl.name, decl.isStackful, &decl, nullptr);
        globalMarkerByName_[decl.name] = marker;
    }
}

uint32_t HirLowerModern::addMarkerMetadata(const session::ModuleArtifact &module,
                                           const std::string_view name, const bool stackful,
                                           const frontend::Declaration *decl,
                                           const frontend::Statement *statement) {
    auto &marker         = hir_.addMarker();
    const auto marker_id = nextMarkerId_++;
    marker.name          = interner_.intern(name);
    marker.marker_id     = marker_id;
    marker.stackful      = stackful;
    marker.body_expr     = statement ? statement->expression.value : (decl ? decl->body.value : 0U);
    markerSources_[marker_id] = SourceMarker{statement, decl};
    if (statement != nullptr)
        markerIdByStmt_[statement->id.value] = marker_id;
    if (decl != nullptr)
        markerIdByDecl_[decl->id.value] = marker_id;

    const auto &params = statement ? statement->parameters : decl->parameters;
    auto *module_sema  = sema_.findModuleSema(module.key);
    for (const auto &param : params) {
        hir::HirMarkerParam hir_param;
        hir_param.name = interner_.intern(param.name);
        if (module_sema != nullptr) {
            const auto sema_type = module_sema->markerParamType(param);
            hir_param.type       = lowerType(sema_type);
        } else {
            hir_param.type = types::kErrorType;
        }
        marker.params.push(std::move(hir_param));
    }
    return marker_id;
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

    auto &hir_fn      = hir_.addFn(interner_.intern(instance.mangled));
    hir_fn.sym_id     = static_cast<symbols::SymId>(functions_.size() + next_sym_id_);
    hir_fn.decl_id    = static_cast<ast::DeclId>(decl->id.value);
    hir_fn.fnSpan     = memory::Span{0, decl->span.start, decl->span.end};
    hir_fn.isVariadic = decl->isVariadic;

    const auto template_type  = module_sema->typeOfDecl(decl->id);
    const auto *template_fn   = sema_.typeTable().function(template_type);
    const auto *instantiation = sema_.instantiations();
    const auto instance_type  = template_fn != nullptr && instantiation != nullptr
                                    ? instantiation->substituteFunction(*template_fn, instance.args)
                                    : kInvalidTypeId;
    const auto *fn            = sema_.typeTable().function(instance_type);
    if (fn != nullptr) {
        hir_fn.return_type = lowerType(fn->result);
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

uint32_t HirLowerModern::resolveMarker(const std::string_view name) const {
    if (const auto *local = marker_decl_stmts_.get(std::string(name))) {
        if (const auto found = markerIdByStmt_.find(*local); found != markerIdByStmt_.end())
            return found->second;
    }
    if (const auto found = globalMarkerByName_.find(std::string(name));
        found != globalMarkerByName_.end())
        return found->second;
    return ~0U;
}

const HirLowerModern::SourceMarker *
HirLowerModern::markerSource(const uint32_t marker_id) const noexcept {
    const auto found = markerSources_.find(marker_id);
    return found == markerSources_.end() ? nullptr : &found->second;
}

size_t HirLowerModern::markerSampleEntry(const uint32_t marker_id) {
    if (const auto found = markerSampleIndex_.find(marker_id); found != markerSampleIndex_.end())
        return markerSamples_[found->second].entry_block;
    const size_t index = markerSamples_.size();
    markerSamples_.push_back(MarkerSample{marker_id, newBlock(), false});
    markerSampleIndex_.emplace(marker_id, index);
    return markerSamples_[index].entry_block;
}

void HirLowerModern::lowerMarkerSamples() {
    // Nested jumps append samples; iterate until the vector stops growing.
    for (size_t index = 0; index < markerSamples_.size(); ++index)
        lowerMarkerSample(markerSamples_[index]);
}

void HirLowerModern::lowerMarkerSample(MarkerSample &sample) {
    if (sample.lowered)
        return;
    sample.lowered             = true;
    const SourceMarker *source = markerSource(sample.marker_id);
    if (source == nullptr)
        return;
    const auto *params = source->statement ? &source->statement->parameters
                                           : (source->decl ? &source->decl->parameters : nullptr);
    current_fn_->blocks[sample.entry_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    setCurrentBlock(sample.entry_block);

    const auto *marker =
        hir_.findMarker(source->statement ? interner_.intern(source->statement->label)
                                          : interner_.intern(source->decl->name));
    if (marker != nullptr && params != nullptr) {
        for (size_t index = 0; index < params->size(); ++index) {
            const auto type =
                index < marker->params.size() ? marker->params[index].type : types::kErrorType;
            hir::HirMarkerLoad load;
            load.marker        = marker->marker_id;
            load.param_index   = static_cast<uint32_t>(index);
            load.type          = type;
            const auto load_id = addExpr(std::move(load));
            current_fn_->blocks[current_block_].insts.push(load_id);
            // Bind the parameter as a normal local slot so expressions referring
            // to the parameter resolve through the existing ResolvedName path.
            const auto slot = localSlot((*params)[index].id);
            current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, type));
            current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, load_id));
        }
    }

    const bool saved_marker = inMarkerBody_;
    inMarkerBody_           = true;
    if (marker != nullptr && marker->body_expr)
        (void)lowerExpr(frontend::ExprId{marker->body_expr});
    inMarkerBody_ = saved_marker;

    if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr) {
        hir::HirMarkerRet ret(arena_);
        for (const auto continuation : markerContinuations_)
            ret.continuations.push(static_cast<hir::HirDeclId>(continuation));
        setTerminator(addExpr(std::move(ret)));
    }
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

    current_fn_            = &hir_.getFn(info.hir_index);
    current_instantiation_ = info.instance != nullptr ? sema_.instantiations() : nullptr;
    current_instance_      = info.instance;
    current_fn_is_flow_ =
        info.decl != nullptr && info.decl->functionKind == frontend::FunctionKind::Flow;

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
    marker_decl_stmts_.clear();
    markerSamples_.clear();
    markerSampleIndex_.clear();
    markerContinuations_.clear();
    inMarkerBody_ = false;
    local_slots_.clear();
    local_slots_.resize(1U);
    current_for_in_binding_stmt_  = {};
    current_for_in_binding_local_ = {};

    current_fn_->blocks.emplace(arena_);
    current_fn_->blocks[0].insts = memory::DynArray<hir::HirExprId>(arena_);

    collectMarkers(info.decl->body);
    for (const auto [marker_name, stmt_id] : marker_decl_stmts_) {
        (void)marker_name;
        if (const auto found = markerIdByStmt_.find(stmt_id); found != markerIdByStmt_.end()) {
            markerSampleEntry(found->second);
        }
    }

    for (size_t index = 0; index < info.decl->parameters.size(); ++index) {
        const auto &parameter = info.decl->parameters[index];
        const auto slot       = localSlot(parameter.id);
        if (nra_ != nullptr) {
            const auto *fact = nra_->localFact(parameter.id);
            if (fact != nullptr && fact->hasResidual()) {
                auto &attrs     = hir_.attrs().slot(slot);
                attrs.ownership = mapHirOwnership(fact->ownership);
                attrs.nonNull   = fact->nonNull;
                attrs.consumed  = fact->knownAlive ? hir::HirConsumedState::NonConsumed
                                                   : hir::HirConsumedState::Consumed;
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
        if (current_fn_->return_type != types::kVoidType && body_expr != hir::kInvalidHirExpr)
            ret.value =
                lowerCoerceToSliceIfArray(current_fn_->return_type, info.decl->body, body_expr);
        current_fn_->blocks[current_block_].terminator = addExpr(std::move(ret));
    }

    lowerMarkerSamples();

    current_module_        = nullptr;
    current_resolution_    = nullptr;
    current_types_         = nullptr;
    current_instantiation_ = nullptr;
    current_instance_      = nullptr;
    current_fn_            = nullptr;
    current_fn_is_flow_    = false;
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
    case TypeKind::Function: {
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
    case frontend::ExprKind::Literal:
        return lowerLiteral(expr, type);
    case frontend::ExprKind::Name:
        return lowerName(expr);
    case frontend::ExprKind::Unary:
        return lowerUnary(expr, type);
    case frontend::ExprKind::Binary:
        return lowerBinary(expr, type);
    case frontend::ExprKind::Call:
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
    if (expr.text == "null" && types_.kindOf(type) == types::TypeKind::Optional) {
        hir::HirMakeNone make_none;
        make_none.type = type;
        return addExpr(std::move(make_none));
    }
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
            const auto slot         = localSlot(resolved->local);
            const auto local_ty     = typeOfLocal(resolved->local);
            const auto expr_type    = typeOfExpr(expr.id);
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
    return addExpr(std::move(var));
}

hir::HirExprId HirLowerModern::lowerUnary(const frontend::Expression &expr,
                                          const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
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
    const frontend::Declaration *method_decl = nullptr;
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
        if (const auto *st = sema_.typeTable().struct_type(pointee)) {
            for (const auto &decl : current_module_->frontend->declarations()) {
                if (decl.kind != frontend::DeclKind::Function)
                    continue;
                if (decl.ownerName != st->name || decl.name != callee_expr.text)
                    continue;
                method_decl = &decl;
                break;
            }
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
        // Insert the implicit self argument only for actual receiver methods.
        // For `.` take the address of the base value, for `->` the base is
        // already a pointer.
        const frontend::ExprId base_id = callee_expr.operands[0];
        hir::HirExprId self_arg        = lowerExpr(base_id);
        if (self_arg == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        if (callee_expr.kind == frontend::ExprKind::Field) {
            const auto base_hir_type = typeOfExpr(base_id);
            const auto self_slot     = next_slot_++;
            current_fn_->blocks[current_block_].insts.push(
                emitSlotAlloca(self_slot, base_hir_type));
            current_fn_->blocks[current_block_].insts.push(emitSlotStore(self_slot, self_arg));
            self_arg = addExpr(hir::HirSlotAddr{self_slot, base_hir_type});
        }
        args.push(self_arg);
        const auto self_type = typeOfExpr(base_id);
        if (self_type != types::kInvalidType)
            arg_types.push(self_type);
    }

    for (size_t index = 1; index < expr.operands.size(); ++index) {
        const size_t call_index = is_receiver_method ? index : index - 1U;
        const auto argument     = lowerExpr(expr.operands[index]);
        if (argument == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        const auto argument_type = typeOfExpr(expr.operands[index]);
        if (argument_type != types::kInvalidType)
            arg_types.push(argument_type);
        const sema::modern::TypeId callee_sema_type = semaTypeOfExpr(callee_id);
        const auto *callee_fn = callee_sema_type != sema::modern::kInvalidTypeId
                                    ? sema_.typeTable().function(callee_sema_type)
                                    : nullptr;
        const auto param_type = callee_fn != nullptr && call_index < callee_fn->params.size()
                                    ? lowerType(callee_fn->params[call_index])
                                    : types::kInvalidType;
        const auto lowered_argument =
            lowerCoerceToSliceIfArray(param_type, expr.operands[index], argument);
        args.push(lowered_argument);
    }

    hir::HirCall call{callee, std::move(args), std::move(arg_types)};
    call.resolved_fn = symbols::kInvalidSym;
    // Indirect calls through a variable (for example a function pointer) must
    // remember the lowered function type so codegen can cast the callee value to
    // a function pointer without re-running semantic analysis.
    if (callee != hir::kInvalidHirExpr) {
        call.fn_type = typeOfExpr(callee_id);
    }
    if (method_decl != nullptr) {
        if (const auto *target = overloadTarget(callee_id);
            target != nullptr && target->module == current_module_->key) {
            method_decl = findDecl(*current_module_, target->decl);
        }
        const auto key = internFunctionKey(interner_, current_module_->key, method_decl->id);
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
        if (call.resolved_fn == symbols::kInvalidSym)
            if (const auto *function_index = function_index_by_key_.get(key))
                call.resolved_fn = functions_[*function_index].sym_id;
    } else if (const auto *target = overloadTarget(callee_id)) {
        // Sema already picked one declaration out of an overload set; re-resolving
        // here would silently fall back to the first candidate.
        const auto key = internFunctionKey(interner_, target->module, target->decl);
        if (const auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
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
        if (call.resolved_fn == symbols::kInvalidSym)
            if (const auto *function_index = function_index_by_key_.get(key))
                call.resolved_fn = functions_[*function_index].sym_id;
    } else if (const auto *resolved = findResolvedExpr(callee_id)) {
        if (const auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
            if (const auto *binding =
                    instantiations->callBinding(current_module_->key, callee_id)) {
                const session::ModuleArtifact *decl_module = nullptr;
                const auto *decl     = resolvedFunctionDecl(*resolved, &decl_module);
                const auto *instance = instantiations->at(binding->instance);
                if (decl != nullptr && decl_module != nullptr && instance != nullptr) {
                    for (const auto &function : functions_) {
                        if (function.instance != nullptr && function.module != nullptr &&
                            function.module->key == decl_module->key &&
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
    const hir::HirExprId call_id = addExpr(std::move(call));
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
    hir::HirExprId last = hir::kInvalidHirExpr;
    for (const auto statement : expr.statements) {
        if (current_fn_->blocks[current_block_].terminator != hir::kInvalidHirExpr)
            break;
        if (!lowerStatement(statement, last))
            return hir::kInvalidHirExpr;
    }
    return last;
}

hir::HirExprId HirLowerModern::lowerIf(const frontend::Expression &expr, const types::TypeId type) {
    if (expr.operands.size() < 2U)
        return hir::kInvalidHirExpr;

    const auto cond = lowerExpr(expr.operands[0]);
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

    setCurrentBlock(then_block);
    current_fn_->blocks[then_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto then_value                 = lowerExpr(expr.operands[1]);
    if (has_value && then_value != hir::kInvalidHirExpr &&
        current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr) {
        current_fn_->blocks[current_block_].insts.push(emitSlotStore(result_slot, then_value));
    }
    if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr)
        emitJump(merge_block);

    if (has_else) {
        setCurrentBlock(else_block);
        current_fn_->blocks[else_block].insts = memory::DynArray<hir::HirExprId>(arena_);
        const auto else_value                 = lowerExpr(expr.operands[2]);
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
        const auto body_value                 = lowerExpr(expr.operands[body_index]);
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

    // A boolean condition is tested directly; any other condition is an equality
    // pattern (`(0)` means `subject == 0`).
    if (types_.kindOf(typeOfExpr(condition)) == types::TypeKind::Bool)
        return lowerExpr(condition);

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
    const auto cond                         = lowerExpr(expr.operands[0]);
    hir::HirBranch branch;
    branch.cond       = cond;
    branch.then_block = static_cast<hir::HirDeclId>(body_block);
    branch.else_block = static_cast<hir::HirDeclId>(exit_block);
    setTerminator(addExpr(std::move(branch)));

    loop_stack_.push_back({header_block, exit_block});
    setCurrentBlock(body_block);
    current_fn_->blocks[body_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    (void)lowerExpr(expr.operands[1]);
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
    const auto cond                         = lowerExpr(expr.operands[0]);
    hir::HirBranch branch;
    branch.cond       = cond;
    branch.then_block = static_cast<hir::HirDeclId>(body_block);
    branch.else_block = static_cast<hir::HirDeclId>(exit_block);
    setTerminator(addExpr(std::move(branch)));

    // `continue` runs the step, so the step is the continue target; `break` exits.
    loop_stack_.push_back({step_block, exit_block});
    setCurrentBlock(body_block);
    current_fn_->blocks[body_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    (void)lowerExpr(expr.operands[1]);
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

    // Keep the iterable alive for the whole loop. A value receiver is copied
    // into a slot and its address is passed as the implicit self argument; a
    // pointer receiver can pass the loaded value directly.
    const auto iterable = lowerExpr(expr.operands[0]);
    if (iterable == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto iterable_type = typeOfExpr(expr.operands[0]);
    const sema::modern::TypeId sema_iterable = semaTypeOfExpr(expr.operands[0]);
    const auto iterable_slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(
        emitSlotAlloca(iterable_slot, iterable_type));
    current_fn_->blocks[current_block_].insts.push(
        emitSlotStore(iterable_slot, iterable));
    const auto iterable_addr = addExpr(hir::HirSlotAddr{iterable_slot, iterable_type});

    const auto *done_ptr =
        current_types_ != nullptr ? current_types_->forInDone.get(expr.id.value) : nullptr;
    const auto *value_ptr =
        current_types_ != nullptr ? current_types_->forInValue.get(expr.id.value) : nullptr;
    const auto *next_ptr =
        current_types_ != nullptr ? current_types_->forInNext.get(expr.id.value) : nullptr;

    const frontend::DeclId done_decl  = done_ptr != nullptr ? *done_ptr : frontend::DeclId{};
    const frontend::DeclId value_decl = value_ptr != nullptr ? *value_ptr : frontend::DeclId{};
    const frontend::DeclId next_decl  = next_ptr != nullptr ? *next_ptr : frontend::DeclId{};

    const auto makeMethodCall = [&](const frontend::DeclId decl_id) -> hir::HirExprId {
        if (!decl_id)
            return hir::kInvalidHirExpr;
        const auto *method_decl = findDecl(*current_module_, decl_id);
        if (method_decl == nullptr)
            return hir::kInvalidHirExpr;
        const auto key = internFunctionKey(interner_, current_module_->key, decl_id);
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
    const auto step_block   = newBlock();
    const auto exit_block   = newBlock();

    emitJump(header_block);

    // Header: `if (iter.done()) break;`.
    setCurrentBlock(header_block);
    current_fn_->blocks[header_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto done_call = makeMethodCall(done_decl);
    if (done_call == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    hir::HirBranch branch;
    branch.cond       = done_call;
    branch.then_block = static_cast<hir::HirDeclId>(exit_block);
    branch.else_block = static_cast<hir::HirDeclId>(body_block);
    setTerminator(addExpr(std::move(branch)));

    // Body: slot = iter.value(); lower the user block.
    setCurrentBlock(body_block);
    current_fn_->blocks[body_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto value_call = makeMethodCall(value_decl);
    if (value_call == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto loop_slot = localSlot(expr.forInBinding);
    const auto loop_type = typeOfLocal(expr.forInBinding);
    current_fn_->blocks[body_block].insts.push(emitSlotAlloca(loop_slot, loop_type));
    current_fn_->blocks[body_block].insts.push(emitSlotStore(loop_slot, value_call));

    loop_stack_.push_back({step_block, exit_block});
    const frontend::StmtId saved_for_in_stmt = current_for_in_binding_stmt_;
    current_for_in_binding_stmt_             = expr.forInBindingStmt;
    current_for_in_binding_local_            = expr.forInBinding;
    (void)lowerExpr(expr.operands[1]);
    current_for_in_binding_stmt_             = saved_for_in_stmt;
    current_for_in_binding_local_            = {};
    if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr)
        emitJump(step_block);
    loop_stack_.pop_back();

    // Step: `iter.next(); goto header;`.
    setCurrentBlock(step_block);
    current_fn_->blocks[step_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto next_call = makeMethodCall(next_decl);
    if (next_call != hir::kInvalidHirExpr)
        current_fn_->blocks[step_block].insts.push(next_call);
    if (current_fn_->blocks[step_block].terminator == hir::kInvalidHirExpr)
        emitJump(header_block);

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
                lowerCoerceToSliceIfArray(typeOfLocal(resolved->local), expr.operands[1], value);
            return emitSlotStore(localSlot(resolved->local), value_slice);
        }
    }
    // For field/arrow lvalue targets, lower the lhs normally (produces HirField) then assign.
    if (lhs_expr.kind == frontend::ExprKind::Field || lhs_expr.kind == frontend::ExprKind::Arrow ||
        lhs_expr.kind == frontend::ExprKind::Index) {
        const auto target_type = typeOfExpr(expr.operands[0]);
        const auto target      = lhs_expr.kind == frontend::ExprKind::Field
                                     ? lowerField(lhs_expr, target_type)
                                 : lhs_expr.kind == frontend::ExprKind::Arrow
                                     ? lowerArrow(lhs_expr, target_type)
                                     : lowerIndex(lhs_expr, target_type);
        const auto value       = lowerExpr(expr.operands[1]);
        if (target == hir::kInvalidHirExpr || value == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        const auto value_slice = lowerCoerceToSliceIfArray(target_type, expr.operands[1], value);
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
        lowerCoerceToSliceIfArray(typeOfExpr(expr.operands[0]), expr.operands[1], value);

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

hir::HirExprId HirLowerModern::lowerCast(const frontend::Expression &expr,
                                         const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    const auto value = lowerExpr(expr.operands[0]);
    if (value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto from = typeOfExpr(expr.operands[0]);
    if (from == type)
        return value;
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
    const auto operand = lowerExpr(expr.operands[0]);
    if (operand == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto operand_type = typeOfExpr(expr.operands[0]);
    if (types_.kindOf(operand_type) != types::TypeKind::Union)
        return hir::kInvalidHirExpr;
    const auto *union_type = std::get_if<types::TypeUnion>(&types_.lookup(operand_type));
    if (union_type == nullptr)
        return hir::kInvalidHirExpr;
    const auto *def = types_.lookupUnionDef(union_type->def_id);
    if (def == nullptr || !def->is_tagged)
        return hir::kInvalidHirExpr;
    const auto target =
        lowerType(sema_.typeTable().lowerTypeExpr(*current_module_->frontend, expr.cast_type));
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
    if (expr.text == "sizeOf")
        intrinsic.which = hir::HirLayoutIntrinsic::Which::SizeOf;
    else if (expr.text == "alignOf")
        intrinsic.which = hir::HirLayoutIntrinsic::Which::AlignOf;
    else
        intrinsic.which = hir::HirLayoutIntrinsic::Which::OffsetOf;
    if (current_module_ == nullptr || current_module_->frontend == nullptr || !expr.cast_type)
        return hir::kInvalidHirExpr;
    const auto &type_exprs = current_module_->frontend->typeExpressions();
    if (expr.cast_type.value > type_exprs.size())
        return hir::kInvalidHirExpr;
    const auto &type_expr           = type_exprs[expr.cast_type.value - 1U];
    const types::TypeId struct_type = lowerType(sema_.typeTable().lookupNamed(type_expr.name));
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

hir::HirExprId HirLowerModern::lowerCoerceToSliceIfArray(types::TypeId target,
                                                         frontend::ExprId expression,
                                                         hir::HirExprId value) {
    if (value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto source_type = typeOfExpr(expression);
    if (types_.kindOf(source_type) != types::TypeKind::Array ||
        types_.kindOf(target) != types::TypeKind::Slice) {
        return value;
    }

    hir::HirMakeSlice slice;
    slice.object      = value;
    slice.type        = target;
    slice.object_type = source_type;
    slice.is_array    = true;
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
    slice.object      = object;
    slice.lo          = lo;
    slice.hi          = hi;
    slice.type        = slice_type;
    slice.object_type = object_type;
    slice.bound_type  = bound_type;
    slice.is_array    = types_.kindOf(object_type) == types::TypeKind::Array;
    slice.checked     = false;
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
    const auto enum_type =
        sema_.typeTable().stripQualifiers(sema_.typeTable().lookupNamed(decl->name));
    const auto *et = sema_.typeTable().enum_type(enum_type);
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
    // `Color.Green` resolves to an enum variant constant, not a struct field read.
    if (const auto variant = enumVariantValue(expr.operands[0], expr.text))
        return addExpr(hir::HirEnumValue{*variant, type});
    const auto object      = lowerExpr(expr.operands[0]);
    const auto object_type = typeOfExpr(expr.operands[0]);
    if (object == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    // Resolve the sema struct type to find the field index by name
    const auto sema_type = sema_.typeTable().stripQualifiers(semaTypeOfExpr(expr.operands[0]));
    const int idx        = sema_.typeTable().fieldIndex(sema_type, expr.text);
    if (idx < 0)
        return hir::kInvalidHirExpr;
    return addExpr(hir::HirField{object, static_cast<uint32_t>(idx), type, object_type});
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
            value = lowerCoerceToSliceIfArray(field_type, expr.operands[i], value);
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
                    : lowerCoerceToSliceIfArray(field_type, default_id, default_value);
        }
    }
    // Keep every slot (missing ones are zero at codegen); the array is index-aligned.
    for (const auto value : ordered)
        lit.values.push(value);
    return addExpr(std::move(lit));
}

hir::HirExprId HirLowerModern::lowerArrayLiteral(const frontend::Expression &expr,
                                                 const types::TypeId type) {
    hir::HirArrayLiteral lit(arena_);
    lit.type = type;
    for (const auto operand : expr.operands) {
        const auto value = lowerExpr(operand);
        if (value == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
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
            if (last_value != hir::kInvalidHirExpr)
                current_fn_->blocks[current_block_].insts.push(last_value);
        }
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
        const auto slot = localSlot(statement.binding.id);
        const auto type = typeOfLocal(statement.binding.id);
        current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, type));
        if (statement.binding.initializer) {
            auto init = lowerExpr(statement.binding.initializer);
            // Coerce T → ?T if the annotation is optional but init is not
            if (init != hir::kInvalidHirExpr && types_.kindOf(type) == types::TypeKind::Optional) {
                const auto init_type = typeOfExpr(statement.binding.initializer);
                if (types_.kindOf(init_type) != types::TypeKind::Optional) {
                    init = lowerCoerceToOptional(type, init);
                }
            }
            init = lowerCoerceToSliceIfArray(type, statement.binding.initializer, init);
            if (init != hir::kInvalidHirExpr)
                current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, init));
        }
        last_value = hir::kInvalidHirExpr;
        return true;
    }
    case frontend::StmtKind::Return: {
        hir::HirRet ret;
        if (statement.expression) {
            auto value = lowerExpr(statement.expression);
            // Coerce T → ?T if return type is optional but value is not
            if (value != hir::kInvalidHirExpr &&
                types_.kindOf(current_fn_->return_type) == types::TypeKind::Optional) {
                const auto val_type = typeOfExpr(statement.expression);
                if (types_.kindOf(val_type) != types::TypeKind::Optional) {
                    value = lowerCoerceToOptional(current_fn_->return_type, value);
                }
            }
            value =
                lowerCoerceToSliceIfArray(current_fn_->return_type, statement.expression, value);
            ret.value = value;
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
        emitJump(loop_stack_.back().break_block);
        setCurrentBlock(newBlock());
        current_fn_->blocks[current_block_].insts = memory::DynArray<hir::HirExprId>(arena_);
        return true;
    case frontend::StmtKind::Continue:
        if (loop_stack_.empty()) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "continue used outside of a loop", {});
            return false;
        }
        emitJump(loop_stack_.back().continue_block);
        setCurrentBlock(newBlock());
        current_fn_->blocks[current_block_].insts = memory::DynArray<hir::HirExprId>(arena_);
        return true;
    case frontend::StmtKind::Marker: {
        if (!current_fn_is_flow_) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "marker is only allowed inside a flow fn", {});
            return false;
        }
        // Local marker declarations are lowered lazily from `jump`/`dock` sites.
        last_value = hir::kInvalidHirExpr;
        return true;
    }
    case frontend::StmtKind::Jump: {
        if (!current_fn_is_flow_) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "jump is only allowed inside a flow fn", {});
            return false;
        }
        const auto marker_id = resolveMarker(statement.label);
        if (marker_id == ~0U) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "jump to undefined marker: '" + statement.label + "'", {});
            return false;
        }
        for (size_t index = 0; index < statement.arguments.size(); ++index) {
            hir::HirMarkerStore store;
            store.marker      = marker_id;
            store.param_index = static_cast<uint32_t>(index);
            store.value       = lowerExpr(statement.arguments[index]);
            if (store.value != hir::kInvalidHirExpr)
                current_fn_->blocks[current_block_].insts.push(addExpr(std::move(store)));
        }
        const auto entry = markerSampleEntry(marker_id);
        hir::HirMarkerJump marker_jump;
        marker_jump.marker_entry = static_cast<hir::HirDeclId>(entry);
        setTerminator(addExpr(std::move(marker_jump)));
        // Anything after a jump is unreachable; give it a fresh block.
        setCurrentBlock(newBlock());
        current_fn_->blocks[current_block_].insts = memory::DynArray<hir::HirExprId>(arena_);
        last_value                                = hir::kInvalidHirExpr;
        return true;
    }
    case frontend::StmtKind::Dock: {
        if (!current_fn_is_flow_) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "dock is only allowed inside a flow fn", {});
            return false;
        }
        const auto marker_id = resolveMarker(statement.label);
        if (marker_id == ~0U) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "dock to undefined marker: '" + statement.label + "'", {});
            return false;
        }
        const size_t continuation = newBlock();
        markerContinuations_.push_back(continuation);
        for (size_t index = 0; index < statement.arguments.size(); ++index) {
            hir::HirMarkerStore store;
            store.marker      = marker_id;
            store.param_index = static_cast<uint32_t>(index);
            store.value       = lowerExpr(statement.arguments[index]);
            current_fn_->blocks[current_block_].insts.push(addExpr(std::move(store)));
        }
        const auto entry = markerSampleEntry(marker_id);
        hir::HirMarkerDock dock;
        dock.marker_entry = static_cast<hir::HirDeclId>(entry);
        dock.continuation = static_cast<hir::HirDeclId>(continuation);
        setTerminator(addExpr(std::move(dock)));
        setCurrentBlock(continuation);
        current_fn_->blocks[continuation].insts = memory::DynArray<hir::HirExprId>(arena_);
        last_value                              = hir::kInvalidHirExpr;
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

void HirLowerModern::collectMarkers(frontend::ExprId id) {
    if (!id || current_module_ == nullptr ||
        id.value > current_module_->frontend->expressions().size())
        return;
    if (current_module_->frontend->isMacroTemplateExpr(id))
        return;
    const auto &expr = current_module_->frontend->expressions()[id.value - 1U];
    // Expanded macro nodes are ordinary expression/statement trees at the
    // call site; marker declarations and uses inside them must be discoverable
    // during the pre-scan just like directly written markers.
    if (expr.kind == frontend::ExprKind::MacroCall && expr.expansion) {
        collectMarkers(expr.expansion);
        return;
    }
    if (expr.kind != frontend::ExprKind::Block)
        return;
    for (const auto &stmt_id : expr.statements) {
        if (!stmt_id || stmt_id.value > current_module_->frontend->statements().size())
            continue;
        const auto &stmt = current_module_->frontend->statements()[stmt_id.value - 1U];
        if (stmt.kind == frontend::StmtKind::Marker && !stmt.label.empty()) {
            marker_decl_stmts_.insert(stmt.label, stmt_id.value);
            addMarkerMetadata(*current_module_, stmt.label, stmt.isStackful, nullptr, &stmt);
        }
        if (stmt.expression)
            collectMarkers(stmt.expression);
    }
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
