#include "symbol-merger.hpp"

#include "diagnostics/error-codes.hpp"

#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>

namespace zith::symbols {

namespace {

auto arena_str(memory::Arena &arena, const std::string &value) -> std::string_view {
    auto *buf = static_cast<char *>(arena.alloc(value.size() + 1));
    std::memcpy(buf, value.data(), value.size());
    buf[value.size()] = '\0';
    return {buf, value.size()};
}

std::string_view last_segment(const std::string &path) {
    auto pos = path.rfind('/');
    if (pos == std::string::npos)
        return path;
    return {path.c_str() + pos + 1, path.size() - pos - 1};
}

struct DeclNames {
    std::string_view qualified;
    std::string_view unqualified;
};

DeclNames makeDeclNames(memory::Arena &arena, std::string_view raw_name, std::string_view ls,
                        std::string_view prefix, bool is_from) {
    if (is_from) {
        auto raw = std::string(raw_name);
        return {
            arena_str(arena, std::string(ls) + "." + raw),
            arena_str(arena, raw),
        };
    }
    return {arena_str(arena, std::string(prefix) + std::string(raw_name)), {}};
}

} // namespace

SymbolMerger::SymbolMerger(memory::Arena &arena, memory::StringInterner &interner,
                           diagnostics::DiagnosticEngine &diags,
                           const memory::DynArray<LoadedFile> &files,
                           memory::DynArray<SymOrigin> &sym_origins)
    : arena_(arena), interner_(interner), diags_(diags), files_(files), sym_origins_(sym_origins) {}

void SymbolMerger::mergeInto(SymbolTable &main_syms, int32_t from_depth) {
    int32_t call_depth = from_depth + 1;

    auto record_origin = [&](SymId main_sid, size_t file_idx, SymId local_sid) {
        if (main_sid >= sym_origins_.size()) {
            if (main_sid >= sym_origins_.capacity())
                sym_origins_.reserve(main_sid + 1);
            while (sym_origins_.size() <= main_sid)
                sym_origins_.push(SymOrigin{size_t(-1), kInvalidSym});
        }
        sym_origins_[main_sid] = SymOrigin{file_idx, local_sid};
    };

    auto report_collision = [&](std::string_view name, SymId existing_sid, size_t incoming_file) {
        if (existing_sid >= sym_origins_.size())
            return;

        const auto &origin = sym_origins_[existing_sid];
        if (origin.file_idx == size_t(-1) || origin.file_idx == incoming_file ||
            origin.file_idx >= files_.size())
            return;

        diags_.report(diagnostics::Severity::Error, diagnostics::err::DuplicateDecl,
                      "imported symbol '" + std::string(name) + "' conflicts between '" +
                          files_[origin.file_idx].import_key + "' and '" +
                          files_[incoming_file].import_key + "'",
                      {});
    };

    std::unordered_set<std::string> from_namespaces;

    for (size_t fi = 0; fi < files_.size(); ++fi) {
        auto &file  = files_[fi];
        auto prefix = file.ns + ".";
        auto ls     = last_segment(file.import_key);

        if (file.is_from && !file.is_asset) {
            auto ls_str = std::string(ls);
            if (!from_namespaces.insert(ls_str).second) {
                diags_.report(diagnostics::Severity::Error, diagnostics::err::DuplicateDecl,
                              "from-namespace '" + ls_str + "' conflicts with another from-import",
                              {});
                continue;
            }
        }

        memory::FlatMap<std::string_view, std::string_view> selective_map;
        std::unordered_set<std::string_view> matched_selective;
        bool selective = !file.import_symbols.empty();
        for (auto &isym : file.import_symbols)
            selective_map[isym.name] = isym.alias;

        auto merge_sym = [&](SymId sid, bool is_module) {
            auto &data = file.symbols.get(sid);
            if (is_module && data.mod_depth >= 0 && call_depth != data.mod_depth)
                return;
            auto vis   = is_module ? SymbolVisibility::Module : SymbolVisibility::Public;
            auto depth = is_module ? data.mod_depth : 0;

            auto raw_name = interner_.lookup(data.name);

            if (selective) {
                auto *alias_ptr = selective_map.get(raw_name);
                if (!alias_ptr)
                    return;
                matched_selective.insert(raw_name);
                auto alias_name = *alias_ptr;

                std::string_view declare_name;
                if (!alias_name.empty())
                    declare_name = arena_str(arena_, std::string(alias_name));
                else
                    declare_name = raw_name;

                auto existing_sid = main_syms.lookupInScope(declare_name, kRootScope);
                if (existing_sid != kInvalidSym) {
                    report_collision(declare_name, existing_sid, fi);
                    return;
                }
                auto main_id = main_syms.declare(declare_name, vis, depth, data.kind, data.decl_id,
                                                 data.span, data.target, data.doc_span);
                record_origin(main_id, fi, sid);
                return;
            }

            auto declare_or_diag = [&](std::string_view name, SymbolVisibility visibility,
                                       int32_t mod_depth, SymId local_sid) {
                auto existing_sid = main_syms.lookupInScope(name, kRootScope);
                if (existing_sid != kInvalidSym) {
                    report_collision(name, existing_sid, fi);
                    return;
                }
                auto main_id =
                    main_syms.declare(name, visibility, mod_depth, data.kind, data.decl_id,
                                      data.span, data.target, data.doc_span);
                record_origin(main_id, fi, local_sid);
            };

            auto names = makeDeclNames(arena_, raw_name, ls, prefix, file.is_from);
            declare_or_diag(names.qualified, vis, depth, sid);
            if (!names.unqualified.empty())
                declare_or_diag(names.unqualified, vis, depth, sid);
        };

        if (file.is_asset) {
            for (auto sid : file.public_syms)
                merge_sym(sid, false);
        } else {
            for (auto sid : file.public_syms)
                merge_sym(sid, false);
            for (auto sid : file.module_syms)
                merge_sym(sid, true);
        }

        if (selective) {
            for (auto &isym : file.import_symbols) {
                if (matched_selective.contains(isym.name))
                    continue;
                diags_.report(diagnostics::Severity::Error, diagnostics::err::ImportError,
                              "import selector '" + std::string(isym.name) +
                                  "' was not found in '" + file.import_key + "'",
                              {});
            }
        }

        std::unordered_set<size_t> visited;
        auto declare_re_export = [&](std::string_view name, SymbolVisibility visibility,
                                     int32_t mod_depth, SymKind kind, ast::DeclId decl_id,
                                     memory::Span span, SymId target, memory::Span doc_span,
                                     size_t re_export_file, SymId local_id) {
            auto existing_sid = main_syms.lookupInScope(name, kRootScope);
            if (existing_sid != kInvalidSym) {
                report_collision(name, existing_sid, re_export_file);
                return;
            }
            auto main_id = main_syms.declare(name, visibility, mod_depth, kind, decl_id, span,
                                             target, doc_span);
            record_origin(main_id, re_export_file, local_id);
        };
        auto process_re_exports = [&](auto &self, size_t re_idx) -> void {
            if (!visited.insert(re_idx).second)
                return;
            auto &ref = files_[re_idx];
            for (auto sid : ref.public_syms) {
                auto &data   = ref.symbols.get(sid);
                auto sym_str = interner_.lookup(data.name);
                auto names   = makeDeclNames(arena_, sym_str, ls, prefix, file.is_from);
                declare_re_export(names.qualified, SymbolVisibility::Public, 0, data.kind,
                                  data.decl_id, data.span, data.target, data.doc_span, re_idx, sid);
                if (!names.unqualified.empty())
                    declare_re_export(names.unqualified, SymbolVisibility::Public, 0, data.kind,
                                      data.decl_id, data.span, data.target, data.doc_span, re_idx,
                                      sid);
            }
            for (auto sid : ref.module_syms) {
                auto &data = ref.symbols.get(sid);
                if (data.mod_depth >= 0 && call_depth != data.mod_depth)
                    continue;
                auto sym_str = interner_.lookup(data.name);
                auto names   = makeDeclNames(arena_, sym_str, ls, prefix, file.is_from);
                declare_re_export(names.qualified, SymbolVisibility::Module, data.mod_depth,
                                  data.kind, data.decl_id, data.span, data.target, data.doc_span,
                                  re_idx, sid);
                if (!names.unqualified.empty())
                    declare_re_export(names.unqualified, SymbolVisibility::Module, data.mod_depth,
                                      data.kind, data.decl_id, data.span, data.target,
                                      data.doc_span, re_idx, sid);
            }
            for (auto re_exported : ref.re_exported_files)
                self(self, re_exported);
        };

        for (auto re_exported : file.re_exported_files)
            process_re_exports(process_re_exports, re_exported);
    }
}

} // namespace zith::symbols
