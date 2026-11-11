#include "sema/hir-lower-modern.hpp"

#include "cinterop/c-header.hpp"
#include "common/overloaded.hpp"
#include "diagnostics/error-codes.hpp"
#include "sema/hir-lower-utils.hpp"
#include "sema/op-mapping.hpp"
#include "sema/sra-facts.hpp"
#include "support/int-literal.hpp"
#include "symbols/symbol-table.hpp"

#include "types/type-canonical.hpp"
#include "types/type-kind.hpp"

#include <algorithm>
#include <cstdlib>
#include <span>
#include <string>

namespace zith::sema {
namespace modern {

HirLowerModern::HirLowerModern(memory::Arena &arena, diagnostics::DiagnosticEngine &diags,
                               const session::CompilationSnapshot &snapshot, SemaPipeline &sema,
                               const symbols::SymbolTable &symbols, types::TypeIntern &types,
                               memory::StringInterner &interner, const SraFacts *sra,
                               cache::Store *cache_store, const std::string_view test_module,
                               const std::string_view test_filter)
    : arena_(arena), diags_(diags), snapshot_(snapshot), sema_(sema), symbols_(symbols),
      types_(types), interner_(interner), sra_(sra), hir_(arena), lowered_types_(),
      function_index_by_key_(), cache_store_(cache_store), test_module_(test_module),
      test_filter_(test_filter) {}

bool HirLowerModern::run() {
    registerForeignTypes();
    // Close the nested-generic instantiation set before predeclaring functions
    // so every concrete callee a generic body needs already exists.
    if (auto *instantiations = sema_.instantiations(); instantiations != nullptr)
        instantiations->concretizeNestedInstances();
    if (!predeclareGlobalConsts() || !predeclareFunctions() || !lowerFunctionBodies() ||
        !buildTestDriver() || diags_.hasErrors())
        return false;
    populateHirDeclarations();
    return !diags_.hasErrors();
}

void HirLowerModern::populateHirDeclarations() {
    memory::FlatMap<symbols::SymId, hir::HirDeclarationId> declaration_ids;
    // Maps the stable `(interned module id, frontend declaration id)` key of a
    // symbol to the HIR declaration it produced. Method lists reference the
    // nested frontend declarations by `DeclId`, so this key lets the producer
    // translate them into HIR declaration ids without ever comparing names.
    memory::FlatMap<uint64_t, hir::HirDeclarationId> declaration_id_by_key;
    for (symbols::SymId symbol_id = 0;
         symbol_id < static_cast<symbols::SymId>(symbols_.symbolCount()); ++symbol_id) {
        const auto &symbol = symbols_.get(symbol_id);

        auto &declaration = hir_.addDeclaration();
        declaration.name  = symbol.name;
        switch (symbol.visibility) {
        case symbols::SymbolVisibility::Private:
            declaration.visibility = hir::HirVisibility::Private;
            break;
        case symbols::SymbolVisibility::Public:
            declaration.visibility = hir::HirVisibility::Public;
            break;
        case symbols::SymbolVisibility::Module:
            declaration.visibility = hir::HirVisibility::Module;
            break;
        }
        declaration.module_depth = symbol.mod_depth;
        declaration.discardable  = symbol.discardable;
        switch (symbol.kind) {
        case symbols::SymKind::Fn:
            declaration.kind = hir::HirDeclarationKind::Function;
            break;
        case symbols::SymKind::Struct:
            declaration.kind = hir::HirDeclarationKind::Struct;
            break;
        case symbols::SymKind::Trait:
            declaration.kind = hir::HirDeclarationKind::Trait;
            break;
        case symbols::SymKind::Interface:
            declaration.kind = hir::HirDeclarationKind::Interface;
            break;
        case symbols::SymKind::Enum:
            declaration.kind = hir::HirDeclarationKind::Enum;
            break;
        case symbols::SymKind::Alias:
            declaration.kind = hir::HirDeclarationKind::Alias;
            break;
        case symbols::SymKind::Variable:
            declaration.kind = hir::HirDeclarationKind::Variable;
            break;
        case symbols::SymKind::Module:
            declaration.kind = hir::HirDeclarationKind::Module;
            break;
        case symbols::SymKind::Component:
            declaration.kind = hir::HirDeclarationKind::Component;
            break;
        case symbols::SymKind::Union:
            declaration.kind = hir::HirDeclarationKind::Union;
            break;
        case symbols::SymKind::Asset:
            declaration.kind = hir::HirDeclarationKind::Asset;
            break;
        case symbols::SymKind::Word:
            declaration.kind = hir::HirDeclarationKind::Word;
            break;
        case symbols::SymKind::Context:
            declaration.kind = hir::HirDeclarationKind::Context;
            break;
        }

        auto type_name = symbol.name;
        if (symbol.kind == symbols::SymKind::Fn && symbol.target != symbols::kInvalidSym) {
            const auto &target = symbols_.get(symbol.target);
            if (target.kind == symbols::SymKind::Variable)
                type_name = target.name;
        }
        const auto type_id = types_.lookupNamedType(interner_.lookup(type_name));
        if (type_id != types::kErrorType)
            declaration.type = type_id;

        if (symbol.source_module_id != memory::kInvalidInternedId && symbol.source_decl_id != 0U) {
            const auto module_key = interner_.lookup(symbol.source_module_id);
            const auto *module    = snapshot_.findModule(module_key);
            const frontend::DeclId source_id{symbol.source_decl_id};
            const auto *source_decl = module != nullptr ? findDecl(*module, source_id) : nullptr;
            const auto *module_sema =
                module != nullptr ? sema_.findModuleSema(module->key) : nullptr;
            if (module != nullptr && source_decl != nullptr && module_sema != nullptr) {
                declaration.is_extern = source_decl->isExtern;

                const auto *saved_module     = current_module_;
                const auto saved_type_module = types_.currentModule();
                current_module_              = module;
                types_.setCurrentModule(module->key);

                const auto sema_type = module_sema->typeOfDecl(source_id);
                if (sema_type) {
                    const auto source_type = lowerType(sema_type);
                    if (source_type != types::kErrorType && source_type != types::kInvalidType)
                        declaration.type = source_type;
                }

                if (source_decl->kind == frontend::DeclKind::Function &&
                    source_decl->genericParams.empty()) {
                    const auto key = internFunctionKey(interner_, module->key, source_id);
                    if (const auto *function_index = function_index_by_key_.get(key)) {
                        const auto function_id = functions_[*function_index].hir_function_id;
                        if (hir_.getFn(function_id).hasBody)
                            declaration.body_function = function_id;
                    }
                }

                if (!source_decl->genericParams.empty()) {
                    auto &template_record = hir_.addTemplate();
                    template_record.declaration_id =
                        static_cast<hir::HirDeclarationId>(hir_.declarationCount() - 1U);
                    template_record.kind      = declaration.kind;
                    template_record.is_extern = source_decl->isExtern;
                    template_record.name      = declaration.name;
                    declaration.template_id =
                        static_cast<hir::HirTemplateId>(hir_.templateCount() - 1U);

                    for (const auto &generic_param : source_decl->genericParams) {
                        auto &param = template_record.params.emplace(arena_);
                        param.name  = interner_.intern(generic_param.name);
                        for (const auto constraint : generic_param.constraints) {
                            const auto bound =
                                sema_.typeTable().lowerTypeExpr(*module->frontend, constraint);
                            const auto lowered_bound = lowerType(bound);
                            if (lowered_bound == types::kErrorType ||
                                lowered_bound == types::kInvalidType) {
                                diags_.reportError(
                                    diagnostics::err::InvalidIR,
                                    "generic bound could not be represented in shared HIR", {});
                                continue;
                            }
                            param.bounds.push(lowered_bound);
                        }
                    }

                    if (source_decl->kind == frontend::DeclKind::Function) {
                        if (const auto *function_type = sema_.typeTable().function(sema_type)) {
                            template_record.return_type = lowerType(function_type->result);
                            template_record.variadic    = source_decl->isVariadic;
                            template_record.is_state =
                                source_decl->functionKind == frontend::FunctionKind::State;
                            template_record.is_foreign_c = source_decl->isExtern;
                            template_record.uses_tail_cc = template_record.is_state;
                            if (source_decl->parameters.size() != function_type->params.size()) {
                                diags_.reportError(
                                    diagnostics::err::InvalidIR,
                                    "generic function parameter names and types have "
                                    "different lengths",
                                    {});
                            }
                            for (size_t index = 0; index < function_type->params.size(); ++index) {
                                if (index >= source_decl->parameters.size()) {
                                    break;
                                }
                                template_record.parameter_types.push(
                                    lowerType(function_type->params[index]));
                                template_record.parameter_names.push(
                                    interner_.intern(source_decl->parameters[index].name));
                            }
                        } else {
                            diags_.reportError(diagnostics::err::InvalidIR,
                                               "generic function has no semantic function type",
                                               {});
                        }
                    } else if (source_decl->kind == frontend::DeclKind::Struct) {
                        if (const auto *struct_type = sema_.typeTable().struct_type(sema_type)) {
                            if (struct_type->fields.size() != struct_type->field_names.size()) {
                                diags_.reportError(
                                    diagnostics::err::InvalidIR,
                                    "generic struct field names and types have different "
                                    "lengths",
                                    {});
                            }
                            for (size_t index = 0; index < struct_type->fields.size(); ++index) {
                                const auto field_name = index < struct_type->field_names.size()
                                                            ? struct_type->field_names[index]
                                                            : std::string_view{};
                                template_record.field_names.push(interner_.intern(field_name));
                                template_record.field_types.push(
                                    lowerType(struct_type->fields[index]));
                            }
                            const auto order = types::canonicalFieldOrder(types_, declaration.type);
                            for (const auto index : order)
                                template_record.canonical_field_order.push(
                                    static_cast<uint32_t>(index));
                        } else {
                            diags_.reportError(diagnostics::err::InvalidIR,
                                               "generic struct has no semantic struct type", {});
                        }
                    }
                }

                current_module_ = saved_module;
                types_.setCurrentModule(saved_type_module);
            }
        }

        declaration_ids[symbol_id] =
            static_cast<hir::HirDeclarationId>(hir_.declarationCount() - 1U);
        if (symbol.source_module_id != memory::kInvalidInternedId && symbol.source_decl_id != 0U) {
            const auto module_key = interner_.lookup(symbol.source_module_id);
            const auto key =
                internFunctionKey(interner_, module_key, frontend::DeclId{symbol.source_decl_id});
            declaration_id_by_key.insert(
                key, static_cast<hir::HirDeclarationId>(hir_.declarationCount() - 1U));
        }
    }

    for (symbols::SymId symbol_id = 0;
         symbol_id < static_cast<symbols::SymId>(symbols_.symbolCount()); ++symbol_id) {
        const auto *declaration_id = declaration_ids.get(symbol_id);
        if (declaration_id == nullptr)
            continue;
        const auto &symbol = symbols_.get(symbol_id);
        if (symbol.kind != symbols::SymKind::Struct && symbol.kind != symbols::SymKind::Union &&
            symbol.kind != symbols::SymKind::Component)
            continue;

        auto &declaration = hir_.getDeclaration(*declaration_id);
        for (const auto member_id : symbol.members) {
            if (const auto *member = declaration_ids.get(member_id))
                declaration.members.push(*member);
        }
    }

    // Populate ordered method lists from the frontend-owned `DeclId` list. The
    // frontend records the ids of the methods written directly inside an
    // owner's own body, so this is an explicit relation, not a name match.
    for (symbols::SymId symbol_id = 0;
         symbol_id < static_cast<symbols::SymId>(symbols_.symbolCount()); ++symbol_id) {
        const auto *declaration_id = declaration_ids.get(symbol_id);
        if (declaration_id == nullptr)
            continue;
        const auto &symbol = symbols_.get(symbol_id);
        if (symbol.source_module_id == memory::kInvalidInternedId || symbol.source_decl_id == 0U)
            continue;
        const auto module_key = interner_.lookup(symbol.source_module_id);
        const auto *module    = snapshot_.findModule(module_key);
        if (module == nullptr)
            continue;
        const auto *source_decl = findDecl(*module, frontend::DeclId{symbol.source_decl_id});
        if (source_decl == nullptr || source_decl->methodDeclarations.empty())
            continue;

        auto &declaration = hir_.getDeclaration(*declaration_id);
        for (const auto method_decl : source_decl->methodDeclarations) {
            const auto key = internFunctionKey(interner_, module_key, method_decl);
            if (const auto *method_id = declaration_id_by_key.get(key))
                declaration.method_declarations.push(*method_id);
        }
        if (declaration.template_id != hir::kInvalidHirTemplate) {
            auto &template_record = hir_.getTemplate(declaration.template_id);
            for (const auto method_id : declaration.method_declarations)
                template_record.method_declarations.push(method_id);
        }
    }

    // Populate a generic owner's trait references from the frontend-owned
    // declaration ids on each implement record. Both the owner and the trait
    // were resolved by the parser to the module's own declarations, so this is
    // an explicit identity relation: never a `traitName`/`ownerName` comparison
    // and never general symbol membership. Only generic owners carry a template
    // record, so non-generic owners contribute no trait reference here.
    for (const auto &module_ptr : snapshot_.modules()) {
        const auto &module = *module_ptr;
        if (module.frontend == nullptr)
            continue;
        auto *module_sema = sema_.findModuleSema(module.key);
        if (module_sema == nullptr)
            continue;
        for (const auto &record : module.frontend->implementRecords()) {
            if (record.ownerDeclaration.value == 0U || record.traitDeclaration.value == 0U)
                continue;
            const auto owner_key =
                internFunctionKey(interner_, module.key, record.ownerDeclaration);
            const auto *owner_id = declaration_id_by_key.get(owner_key);
            if (owner_id == nullptr)
                continue;
            const auto template_id = hir_.getDeclaration(*owner_id).template_id;
            if (template_id == hir::kInvalidHirTemplate)
                continue;
            const auto trait_sema = module_sema->typeOfDecl(record.traitDeclaration);
            if (!trait_sema) {
                diags_.reportError(diagnostics::err::InvalidIR,
                                   "implement record trait declaration has no semantic type", {});
                continue;
            }
            const auto trait_type = lowerType(trait_sema);
            if (trait_type == types::kErrorType || trait_type == types::kInvalidType ||
                std::get_if<types::TypeTrait>(&types_.lookup(trait_type)) == nullptr) {
                diags_.reportError(
                    diagnostics::err::InvalidIR,
                    "implement record trait could not be represented as a shared HIR trait", {});
                continue;
            }
            auto &template_record = hir_.getTemplate(template_id);
            bool already_present  = false;
            for (const auto existing : template_record.trait_types) {
                if (existing == trait_type) {
                    already_present = true;
                    break;
                }
            }
            if (!already_present)
                template_record.trait_types.push(trait_type);
        }
    }
}

void HirLowerModern::registerForeignTypes() {
    for (const auto &header : snapshot_.cHeaders()) {
        if (header == nullptr)
            continue;
        std::vector<const cinterop::Type *> pending;
        const auto queue = [&](const cinterop::Type &type) {
            pending.push_back(&type);
        };
        for (const auto &function : header->functions) {
            queue(function.result);
            for (const auto &parameter : function.parameters)
                queue(parameter);
        }
        for (const auto &global : header->globals)
            queue(global.type);
        for (const auto &type_alias : header->typedefs)
            queue(type_alias.type);
        while (!pending.empty()) {
            const auto *type = pending.back();
            pending.pop_back();
            if (type == nullptr)
                continue;
            if (type->kind == cinterop::TypeKind::Record) {
                const auto key = interner_.intern(type->name);
                if (foreign_record_types_.get(key) == nullptr)
                    foreign_record_types_.insert(key, type);
                for (const auto &field : type->recordFields) {
                    if (field.type != nullptr)
                        pending.push_back(field.type.get());
                }
            } else if (type->kind == cinterop::TypeKind::Enum) {
                const auto key = interner_.intern(type->name);
                if (foreign_enum_types_.get(key) == nullptr)
                    foreign_enum_types_.insert(key, type);
                if (type->enumUnderlying != nullptr)
                    pending.push_back(type->enumUnderlying.get());
            } else if (type->kind == cinterop::TypeKind::Pointer && type->pointee != nullptr) {
                pending.push_back(type->pointee.get());
            }
        }
    }
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
                decl.bindingKind != frontend::BindingKind::Const || decl.isExtern ||
                decl.name.empty())
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
            for (const auto &attribute : decl.attributes) {
                if (attribute.kind != frontend::AttributeKind::AlignAs)
                    continue;
                uint32_t alignment = 0;
                if (module_sema->getRequestedAlignment(attribute.argument, alignment))
                    global.alignmentBytes = alignment;
            }
            if (decl.initializer) {
                current_module_     = &module;
                current_resolution_ = snapshot_.findResolution(module.key);
                current_types_      = sema_.findTypedMap(module.key);
                types_.setCurrentModule(module.key);
                global.init         = lowerExpr(decl.initializer);
                current_module_     = nullptr;
                current_resolution_ = nullptr;
                current_types_      = nullptr;
                types_.setCurrentModule({});
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
            hir_fn.fnSpan     = memory::Span{0, decl.span.start, decl.span.end};
            hir_fn.isVariadic = decl.isVariadic;
            hir_fn.isState    = decl.functionKind == frontend::FunctionKind::State;
            if (decl.discardable())
                hir_.attrs().fn(hir_.getFnCount() - 1U).discardable = true;
            if (!decl.parameters.empty() && decl.parameters.back().isVariadicSlice)
                hir_fn.variadicSliceParam = decl.parameters.size() - 1U;

            bool is_main_void  = false;
            bool is_test_void  = false;
            const auto fn_type = module_sema->typeOfDecl(decl.id);
            if (const auto *fn = sema_.typeTable().function(fn_type)) {
                hir_fn.return_type = lowerType(fn->result);
                is_main_void = decl.name == "main" && !decl.isExtern && decl.ownerName.empty() &&
                               decl.visibility != frontend::Visibility::Public &&
                               module.key == snapshot_.rootModuleKey() &&
                               hir_fn.return_type == types::kVoidType;
                if (is_main_void)
                    hir_fn.return_type = types_.internInt(types::IntWidth::I32);
                is_test_void = decl.isTest() && !decl.isExtern && decl.ownerName.empty() &&
                               module.key == snapshot_.rootModuleKey() &&
                               hir_fn.return_type == types::kVoidType;
                if (is_test_void)
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
            if (is_test_void) {
                test_names_.push_back(decl.name);
                test_fn_ids_.push_back(static_cast<hir::HirFunctionId>(hir_.getFnCount() - 1U));
            }
            functions_.push_back({key, module_ptr.get(), &decl, nullptr, nullptr,
                                  static_cast<hir::HirFunctionId>(hir_.getFnCount() - 1U),
                                  hir_.getFnCount() - 1U, is_main_void, is_test_void});
        }
    }
    if (const auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
        for (size_t index = 0; index < instantiations->instanceCount(); ++index) {
            const auto *instance = instantiations->at(index);
            // Abstract instances (`reserve<T, T>`) are template artifacts left
            // over from the reified generic body. They have no concrete
            // signature to emit; only the concretized variants are callable.
            if (instance != nullptr && !instantiations->instanceIsAbstract(*instance))
                predeclareInstantiation(instance->module, *instance);
        }
    }
    for (const auto &header : snapshot_.cHeaders()) {
        if (header == nullptr)
            continue;
        for (const auto &foreign : header->functions) {
            const auto &function_name =
                foreign.adapterThunkName.empty() ? foreign.linkageName : foreign.adapterThunkName;
            auto &hir_fn      = hir_.addFn(interner_.intern(function_name));
            hir_fn.isForeignC = true;
            if (!foreign.adapterName.empty())
                hir_fn.foreignAdapterName = interner_.intern(foreign.adapterName);
            hir_fn.return_type = lowerForeignType(foreign.result);
            hir_fn.isVariadic  = foreign.isVariadic;
            for (const auto &parameter : foreign.parameters) {
                hir_fn.params.push(lowerForeignType(parameter));
                hir_fn.param_names.push({});
            }
            functions_.push_back({0, nullptr, nullptr, &foreign, nullptr,
                                  static_cast<hir::HirFunctionId>(hir_.getFnCount() - 1U),
                                  hir_.getFnCount() - 1U});
        }
    }
    return true;
}

bool HirLowerModern::lowerFunctionBodies() {
    for (auto &function : functions_) {
        // Skip abstract instances: their bodies call other abstract symbols and
        // cannot be lowered without a concrete argument tuple.
        if (function.instance != nullptr) {
            const auto *instantiations = sema_.instantiations();
            if (instantiations != nullptr && instantiations->instanceIsAbstract(*function.instance))
                continue;
        }
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
    hir_fn.fnSpan     = memory::Span{0, decl->span.start, decl->span.end};
    hir_fn.isVariadic = decl->isVariadic;
    hir_fn.isState    = decl->functionKind == frontend::FunctionKind::State;
    if (decl->discardable())
        hir_.attrs().fn(hir_.getFnCount() - 1U).discardable = true;
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
        const auto *saved_module     = current_module_;
        const auto saved_type_module = types_.currentModule();
        current_module_              = module;
        types_.setCurrentModule(module_key);

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

        current_module_ = saved_module;
        types_.setCurrentModule(saved_type_module);
    }

    const auto key = internFunctionKey(interner_, module_key, instance.decl);
    // The first lowered concrete symbol for a generic decl is the canonical
    // target; call-site bindings identify the exact instance, so the key also
    // needs to encode the type-argument tuple when multiple instances exist.
    if (!function_index_by_key_.contains(key))
        function_index_by_key_.insert(key, hir_.getFnCount() - 1U);
    functions_.push_back({key, module, decl, nullptr, &instance,
                          static_cast<hir::HirFunctionId>(hir_.getFnCount() - 1U),
                          hir_.getFnCount() - 1U});
}

std::vector<sema::modern::TypeId>
HirLowerModern::concreteInstanceArgs(const comptime::InstantiationInstance &instance) const {
    const auto *instantiations = sema_.instantiations();
    if (instantiations == nullptr || current_instance_ == nullptr)
        return instance.args;
    return instantiations->substituteArgs(instance.args, current_instance_->args);
}

hir::HirFunctionId
HirLowerModern::findInstanceFunction(const session::ModuleKey &module, const frontend::DeclId decl,
                                     const std::vector<sema::modern::TypeId> &args) const noexcept {
    for (const auto &function : functions_) {
        if (function.instance != nullptr && function.module != nullptr &&
            function.module->key == module && function.instance->decl == decl &&
            function.instance->args == args)
            return function.hir_function_id;
    }
    return hir::kInvalidHirFunction;
}

hir::HirFunctionId
HirLowerModern::concretizeCallTarget(const comptime::InstantiationInstance &bound_instance) {
    // A call bound inside a generic body records the enclosing function's type
    // parameters (`reserve<K, V>` inside `HashMap<K, V>`). Substitute the
    // concrete arguments of the instance currently being lowered so the call
    // resolves to `reserve<i64, i64>` instead of the abstract template symbol.
    const std::vector<sema::modern::TypeId> concrete = concreteInstanceArgs(bound_instance);
    return findInstanceFunction(bound_instance.module, bound_instance.decl, concrete);
}

bool HirLowerModern::lowerFunctionBody(FunctionInfo &info) {
    current_module_     = info.module;
    current_resolution_ = snapshot_.findResolution(info.module->key);
    current_types_      = sema_.findTypedMap(info.module->key);
    types_.setCurrentModule(info.module->key);
    if (current_resolution_ == nullptr || current_types_ == nullptr) {
        diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                      "missing semantic data for module '" + info.module->key + "'", {});
        return false;
    }

    current_fn_                  = &hir_.getFn(info.hir_index);
    current_fn_index_            = info.hir_index;
    current_fn_->hasBody         = true;
    current_instantiation_       = info.instance != nullptr ? sema_.instantiations() : nullptr;
    current_instance_            = info.instance;
    current_fn_return_sema_type_ = sema::modern::kInvalidTypeId;
    current_fn_decl_             = info.decl;
    if (const auto *module_sema = sema_.findModuleSema(info.module->key);
        module_sema != nullptr && info.decl != nullptr) {
        sema::modern::TypeId fn_sema_type = module_sema->typeOfDecl(info.decl->id);
        if (const auto *fn = sema_.typeTable().function(fn_sema_type))
            // The declared result carries the generic parameters (`?V`); the
            // instance body must coerce against the reified result (`?i64`),
            // otherwise optional-return wrapping never fires and the bare
            // value fails IR verification against the aggregate return type.
            current_fn_return_sema_type_ =
                current_instantiation_ != nullptr && current_instance_ != nullptr
                    ? current_instantiation_->substituteType(fn->result, current_instance_->args)
                    : fn->result;
    }
    info_decl_parent_scope_ = info.decl != nullptr ? info.decl->parentScope : frontend::ScopeId{};
    current_fn_is_state_ =
        info.decl != nullptr && info.decl->functionKind == frontend::FunctionKind::State;
    current_state_machine_id_ = current_fn_is_state_ ? current_fn_->machineId : 0;

    if (sra_ != nullptr && info.module != nullptr && info.decl != nullptr) {
        const auto *fn_fact = sra_->functionFact(info.module->key, info.decl->id);
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
    current_main_void_            = is_main_void;
    current_fn_is_test_           = info.forced_test_return_i32;
    current_test_name_            = current_fn_is_test_ ? info.decl->name : std::string{};
    current_test_failure_id_      = 0;

    current_fn_->blocks.emplace(arena_);
    current_fn_->blocks[0].insts = memory::DynArray<hir::HirExprId>(arena_);

    for (size_t index = 0; index < info.decl->parameters.size(); ++index) {
        const auto &parameter = info.decl->parameters[index];
        const auto slot       = localSlot(parameter.id);
        current_fn_->param_slots.push(slot);
        // The slot id is read by codegen from param_slots; publish attrs for
        // parameters even when the function body never names them, because the
        // ABI-level ownership decision depends on the parameter itself.
        if (sra_ != nullptr) {
            const auto *fact = sra_->localFact(parameter.id);
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
        } else if (current_fn_is_test_) {
            // Check failures are reported through the test failure-event stream.
            ret.value = emitI32Literal(0);
        } else if (current_fn_->return_type != types::kVoidType &&
                   body_expr != hir::kInvalidHirExpr) {
            if (types_.kindOf(current_fn_->return_type) == types::TypeKind::Optional &&
                info.decl->body) {
                const auto val_sema_type = semaTypeOfExpr(info.decl->body);
                // A `T -> ?dyn`/`?opaque` return erases the payload before the
                // `Some` wrap. When it applies, the depth wrap below is skipped
                // so the erased payload is not wrapped a second time.
                if (current_fn_return_sema_type_ != sema::modern::kInvalidTypeId &&
                    val_sema_type != sema::modern::kInvalidTypeId)
                    ret.value = lowerCoerceToOptionalErased(current_fn_->return_type,
                                                            current_fn_return_sema_type_,
                                                            info.decl->body, body_expr);
                if (ret.value == hir::kInvalidHirExpr &&
                    current_fn_return_sema_type_ != sema::modern::kInvalidTypeId &&
                    val_sema_type != sema::modern::kInvalidTypeId)
                    ret.value = lowerCoerceToOptionalDepth(current_fn_->return_type,
                                                           current_fn_return_sema_type_,
                                                           val_sema_type, body_expr);
                else if (ret.value == hir::kInvalidHirExpr &&
                         sema_.typeTable().kindOf(val_sema_type) != TypeKind::Optional)
                    ret.value = lowerCoerceToOptional(current_fn_->return_type, body_expr);
                else if (ret.value == hir::kInvalidHirExpr) {
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

    current_module_     = nullptr;
    current_resolution_ = nullptr;
    current_types_      = nullptr;
    types_.setCurrentModule({});
    current_instantiation_       = nullptr;
    current_instance_            = nullptr;
    current_fn_return_sema_type_ = sema::modern::kInvalidTypeId;
    info_decl_parent_scope_      = {};
    current_fn_                  = nullptr;
    current_fn_is_state_         = false;
    current_state_machine_id_    = 0;
    current_main_void_           = false;
    current_fn_is_test_          = false;
    current_test_name_.clear();
    current_test_failure_id_ = 0;
    return true;
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

hir::HirFunctionId
HirLowerModern::resolvedFunctionId(const session::ResolvedName &resolved) const noexcept {
    if (resolved.foreignFunction != nullptr) {
        for (const auto &function : functions_) {
            if (function.foreign == resolved.foreignFunction)
                return function.hir_function_id;
        }
        return hir::kInvalidHirFunction;
    }
    const session::ModuleArtifact *module = nullptr;
    const auto *decl                      = resolvedFunctionDecl(resolved, &module);
    if (decl == nullptr || module == nullptr)
        return hir::kInvalidHirFunction;
    const auto key = internFunctionKey(interner_, module->key, decl->id);
    if (const auto *function_index = function_index_by_key_.get(key))
        return functions_[*function_index].hir_function_id;
    return hir::kInvalidHirFunction;
}

hir::HirSlotId HirLowerModern::localSlot(frontend::LocalId id) {
    if (!id)
        return hir::kInvalidHirSlot;
    if (id.value >= local_slots_.size())
        local_slots_.resize(id.value + 1U, hir::kInvalidHirSlot);
    if (local_slots_[id.value] == hir::kInvalidHirSlot) {
        local_slots_[id.value] = next_slot_++;
        if (sra_ != nullptr && sra_->localIsVolatile(id))
            hir_.attrs().slot(local_slots_[id.value]).volatileSlot = true;
        if (sra_ != nullptr) {
            const auto *fact = sra_->localFact(id);
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

hir::HirExprId HirLowerModern::addExpr(hir::HirExpr expr) {
    return hir_.addExpr(std::move(expr));
}

hir::HirExprId HirLowerModern::emitSlotAlloca(const hir::HirSlotId slot, const types::TypeId type,
                                              const uint32_t alignment_bytes) {
    return addExpr(hir::HirSlotAlloca{slot, type, alignment_bytes});
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
    jump.target = static_cast<hir::HirBlockId>(target);
    setTerminator(addExpr(std::move(jump)));
}

} // namespace modern
} // namespace zith::sema
