#include "hir-module.hpp"
#include "common/overloaded.hpp"
#include "hir/hir-expr.hpp"

#include <cstdio>
#include <string>

namespace zith::hir {

namespace {} // namespace

HirModule::HirModule(memory::Arena &arena)
    : exprs_(arena), fns_(arena), globals_(arena), vtables_(arena), attrs_(arena) {}

HirExprId HirModule::addExpr(HirExpr expr) {
    HirExprId id = static_cast<HirExprId>(exprs_.size());
    exprs_.push(std::move(expr));
    return id;
}

HirFunction &HirModule::addFn(memory::InternedId name) {
    fns_.emplace(exprs_.arena());
    fns_.back().name = name;
    return fns_.back();
}

HirFunction &HirModule::getFn(size_t idx) {
    return fns_[idx];
}
const HirExpr &HirModule::getExpr(HirExprId id) const {
    return exprs_[id];
}
HirExpr &HirModule::getExprMut(HirExprId id) {
    return exprs_[id];
}
const HirFunction &HirModule::getFn(size_t idx) const {
    return fns_[idx];
}

HirGlobalConst &HirModule::addGlobalConst() {
    globals_.emplace(exprs_.arena());
    return globals_.back();
}

const HirGlobalConst &HirModule::getGlobalConst(size_t idx) const {
    return globals_[idx];
}

HirVTable &HirModule::addVTable(memory::InternedId name) {
    vtables_.emplace(exprs_.arena());
    vtables_.back().name = name;
    return vtables_.back();
}

const HirVTable &HirModule::getVTable(size_t idx) const {
    return vtables_[idx];
}

size_t HirModule::getFnCount() const {
    return fns_.size();
}

void HirModule::dump(FILE *out, const memory::StringInterner &interner) const {
    const std::string text = toString(interner);
    std::fwrite(text.data(), 1, text.size(), out);
}

std::string HirModule::toString(const memory::StringInterner &interner) const {
    std::string buffer;
    for (const auto &global : globals_) {
        auto name = interner.lookup(global.name);
        buffer += "global_const ";
        buffer.append(name.data(), name.size());
        buffer += " : %t";
        buffer += std::to_string(global.type);
        buffer += " = %e";
        buffer += std::to_string(global.init);
        buffer += "\n";
    }
    if (!globals_.empty())
        buffer += "\n";
    for (const auto &vtable : vtables_) {
        auto name = interner.lookup(vtable.name);
        buffer += "vtable ";
        buffer.append(name.data(), name.size());
        buffer += "(";
        for (size_t si = 0; si < vtable.slots.size(); ++si) {
            if (si > 0)
                buffer += ", ";
            buffer += "<fn ";
            buffer += std::to_string(static_cast<uint64_t>(vtable.slots[si]));
            buffer += ">";
        }
        buffer += ")\n";
    }
    if (!vtables_.empty())
        buffer += "\n";
    for (size_t fi = 0; fi < fns_.size(); ++fi) {
        auto &fn     = fns_[fi];
        auto fn_name = interner.lookup(fn.name);
        if (fn.blocks.empty()) {
            buffer += "fn ";
            buffer.append(fn_name.data(), fn_name.size());
            buffer += " : extern\n";
            continue;
        }
        buffer += "fn ";
        buffer.append(fn_name.data(), fn_name.size());
        buffer += "(";
        for (size_t pi = 0; pi < fn.params.size(); ++pi) {
            if (pi > 0)
                buffer += ", ";
            buffer += "%p";
            buffer += std::to_string(pi);
        }
        buffer += ")";
        buffer += " -> %t";
        buffer += std::to_string(fn.return_type);
        if (fn.isState) {
            buffer += " state m";
            buffer += std::to_string(fn.machineId);
        }
        buffer += " {\n";
        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            auto &block = fn.blocks[bi];
            buffer += "bb";
            buffer += std::to_string(bi);
            buffer += ":\n";
            for (auto inst_id : block.insts) {
                buffer += "  %e";
                buffer += std::to_string(inst_id);
                buffer += " = ";
                auto &expr = exprs_[inst_id];
                hir::visitExpr(expr,
                               common::overloaded{
                                   [&](const HirLiteral &lit) {
                                       if (lit.type == static_cast<HirTypeId>(-1))
                                           buffer += "literal<?>";
                                       else
                                           buffer += "literal %t";
                                       buffer += std::to_string(lit.type);
                                   },
                                   [&](const HirBinary &bin) {
                                       buffer += "binary %e";
                                       buffer += std::to_string(bin.lhs);
                                       buffer += " op %e";
                                       buffer += std::to_string(bin.rhs);
                                   },
                                   [&](const HirUnary &un) {
                                       buffer += "unary %t";
                                       buffer += std::to_string(un.type);
                                       buffer += " op %e";
                                       buffer += std::to_string(un.operand);
                                   },
                                   [&](const HirLet &let) {
                                       auto n = interner.lookup(let.name);
                                       buffer += "let ";
                                       buffer.append(n.data(), n.size());
                                   },
                                   [&](const HirVar &var) {
                                       auto n = interner.lookup(var.name);
                                       buffer += "var ";
                                       buffer.append(n.data(), n.size());
                                       buffer += "#";
                                       buffer += std::to_string(var.version);
                                   },
                                   [&](const HirCall &call) {
                                       if (call.callee != kInvalidHirExpr) {
                                           auto &callee = exprs_[call.callee];
                                           if (auto *calleeVar = std::get_if<HirVar>(&callee)) {
                                               auto n = interner.lookup(calleeVar->name);
                                               buffer += "call ";
                                               buffer.append(n.data(), n.size());
                                               buffer += "(";
                                           } else {
                                               buffer += "call %e";
                                               buffer += std::to_string(call.callee);
                                               buffer += "(";
                                           }
                                       } else {
                                           buffer += "call <resolved>(";
                                       }
                                       for (size_t ai = 0; ai < call.args.size(); ++ai) {
                                           if (ai > 0)
                                               buffer += ", ";
                                           buffer += "%e";
                                           buffer += std::to_string(call.args[ai]);
                                       }
                                       buffer += ")";
                                       if (call.musttail)
                                           buffer += " musttail";
                                       if (call.fn_type != types::kInvalidType) {
                                           buffer += " fn_type %t";
                                           buffer += std::to_string(call.fn_type);
                                       }
                                   },
                                   [&](const HirRet &ret) {
                                       if (ret.value == kInvalidHirExpr)
                                           buffer += "ret void";
                                       else
                                           buffer += "ret %e";
                                       buffer += std::to_string(ret.value);
                                   },
                                   [&](const HirJump &jump) {
                                       buffer += "jump bb";
                                       buffer += std::to_string(jump.target);
                                   },
                                   [&](const HirBranch &branch) {
                                       buffer += "branch %e";
                                       buffer += std::to_string(branch.cond);
                                       buffer += " -> bb";
                                       buffer += std::to_string(branch.then_block);
                                       buffer += " : bb";
                                       buffer += std::to_string(branch.else_block);
                                   },
                                   [&](const HirPhi &) { buffer += "<expr>"; },
                                   [&](const HirAssign &assign) {
                                       buffer += "assign %e";
                                       buffer += std::to_string(assign.target);
                                       buffer += " = %e";
                                       buffer += std::to_string(assign.value);
                                   },
                                   [&](const HirIndex &idx) {
                                       buffer += "index %e";
                                       buffer += std::to_string(idx.object);
                                       buffer += "[%e";
                                       buffer += std::to_string(idx.index);
                                       buffer += "]";
                                   },
                                   [&](const HirField &field) {
                                       buffer += "field %e";
                                       buffer += std::to_string(field.object);
                                       buffer += ".";
                                       buffer += std::to_string(field.index);
                                   },
                                   [&](const HirStructLiteral &literal) {
                                       buffer += "struct_literal %t";
                                       buffer += std::to_string(literal.type);
                                   },
                                   [&](const HirArrayLiteral &literal) {
                                       buffer += "array_literal %t";
                                       buffer += std::to_string(literal.type);
                                   },
                                   [&](const HirEnumValue &value) {
                                       buffer += "enum_value %t";
                                       buffer += std::to_string(value.type);
                                   },
                                   [&](const HirSlotAlloca &s) {
                                       buffer += "slot_alloca s";
                                       buffer += std::to_string(s.slot);
                                       buffer += " : %t";
                                       buffer += std::to_string(s.type);
                                   },
                                   [&](const HirSlotStore &s) {
                                       buffer += "slot_store s";
                                       buffer += std::to_string(s.slot);
                                       buffer += " = %e";
                                       buffer += std::to_string(s.value);
                                   },
                                   [&](const HirSlotLoad &s) {
                                       buffer += "slot_load s";
                                       buffer += std::to_string(s.slot);
                                       buffer += " : %t";
                                       buffer += std::to_string(s.type);
                                   },
                                   [&](const HirSlotAddr &s) {
                                       buffer += "slot_addr s";
                                       buffer += std::to_string(s.slot);
                                       buffer += " : %t";
                                       buffer += std::to_string(s.type);
                                   },
                                   [&](const HirMakeNone &m) {
                                       buffer += "make_none : %t";
                                       buffer += std::to_string(m.type);
                                   },
                                   [&](const HirMakeSome &m) {
                                       buffer += "make_some %e";
                                       buffer += std::to_string(m.value);
                                       buffer += " : %t";
                                       buffer += std::to_string(m.type);
                                   },
                                   [&](const HirMakeSlice &slice) {
                                       buffer += "make_slice %e";
                                       buffer += std::to_string(slice.object);
                                       buffer += "[%e";
                                       buffer += std::to_string(slice.lo);
                                       buffer += "..%e";
                                       buffer += std::to_string(slice.hi);
                                       buffer += "] : %t";
                                       buffer += std::to_string(slice.type);
                                   },
                                   [&](const HirUnionCast &c) {
                                       buffer += "union_cast %e";
                                       buffer += std::to_string(c.value);
                                       buffer += " : %t";
                                       buffer += std::to_string(c.from);
                                       buffer += " -> %t";
                                       buffer += std::to_string(c.to);
                                   },
                                   [&](const HirUnionCheck &c) {
                                       buffer += "union_check %e";
                                       buffer += std::to_string(c.value);
                                       buffer += " member ";
                                       buffer += std::to_string(c.member_index);
                                   },
                                   [&](const HirCast &c) {
                                       buffer += "cast %e";
                                       buffer += std::to_string(c.value);
                                       buffer += " : %t";
                                       buffer += std::to_string(c.from);
                                       buffer += " -> %t";
                                       buffer += std::to_string(c.to);
                                   },
                                   [&](const HirLayoutIntrinsic &i) {
                                       switch (i.which) {
                                       case HirLayoutIntrinsic::Which::LengthOf:
                                           buffer += "length_of %e";
                                           buffer += std::to_string(i.operand);
                                           if (i.string_length != 0U) {
                                               buffer += " = ";
                                               buffer += std::to_string(i.string_length);
                                           }
                                           buffer += " : %t";
                                           buffer += std::to_string(i.type);
                                           break;
                                       case HirLayoutIntrinsic::Which::PtrOf:
                                           buffer += "ptr_of %e";
                                           buffer += std::to_string(i.operand);
                                           buffer += " : %t";
                                           buffer += std::to_string(i.type);
                                           break;
                                       case HirLayoutIntrinsic::Which::OffsetOf:
                                           buffer += "offset_of %t";
                                           buffer += std::to_string(i.type);
                                           break;
                                       case HirLayoutIntrinsic::Which::AlignOf:
                                           buffer += "align_of %t";
                                           buffer += std::to_string(i.type);
                                           break;
                                       case HirLayoutIntrinsic::Which::SizeOf:
                                           buffer += "size_of %t";
                                           buffer += std::to_string(i.type);
                                           break;
                                       }
                                   },
                                   [&](const HirCanonicalType &canonical) {
                                       buffer += "canonical_type %t";
                                       buffer += std::to_string(canonical.type);
                                       buffer += " = ";
                                       buffer += std::to_string(canonical.canonical_id.hi);
                                       buffer += ":";
                                       buffer += std::to_string(canonical.canonical_id.lo);
                                   },
                                   [&](const HirStateTailCall &tail) {
                                       if (tail.call.resolved_fn != symbols::kInvalidSym)
                                           buffer += "state_tail_call <resolved>(";
                                       else
                                           buffer += "state_tail_call %e";
                                       buffer += std::to_string(tail.call.callee);
                                       buffer += "(";
                                       for (size_t ai = 0; ai < tail.call.args.size(); ++ai) {
                                           if (ai > 0)
                                               buffer += ", ";
                                           buffer += "%e";
                                           buffer += std::to_string(tail.call.args[ai]);
                                       }
                                       buffer += ")";
                                   },
                                   [&](const HirCleanup &cleanup) {
                                       buffer += "cleanup(";
                                       for (size_t ai = 0; ai < cleanup.exprs.size(); ++ai) {
                                           if (ai > 0)
                                               buffer += ", ";
                                           buffer += "%e";
                                           buffer += std::to_string(cleanup.exprs[ai]);
                                       }
                                       buffer += ")";
                                   },
                                   [&](const HirGlobalConstLoad &g) {
                                       auto n = interner.lookup(g.name);
                                       buffer += "global_const_load ";
                                       buffer.append(n.data(), n.size());
                                       buffer += " : %t";
                                       buffer += std::to_string(g.type);
                                   },
                                   [&](const HirMakeDyn &m) {
                                       auto n = interner.lookup(m.vtable_name);
                                       buffer += "make_dyn %e";
                                       buffer += std::to_string(m.value);
                                       buffer += " : %t";
                                       buffer += std::to_string(m.source_type);
                                       buffer += " -> %t";
                                       buffer += std::to_string(m.dyn_type);
                                       buffer += " vtable ";
                                       buffer.append(n.data(), n.size());
                                   },
                                   [&](const HirDynCall &call) {
                                       auto n = interner.lookup(call.vtable_name);
                                       buffer += "dyn_call %e";
                                       buffer += std::to_string(call.receiver);
                                       buffer += " [";
                                       buffer += std::to_string(call.slot_index);
                                       buffer += "] vtable ";
                                       buffer.append(n.data(), n.size());
                                       buffer += "(";
                                       for (size_t ai = 0; ai < call.args.size(); ++ai) {
                                           if (ai > 0)
                                               buffer += ", ";
                                           buffer += "%e";
                                           buffer += std::to_string(call.args[ai]);
                                       }
                                       buffer += ")";
                                   },
                                   [&](const HirMakeOpaque &m) {
                                       buffer += "make_opaque %e";
                                       buffer += std::to_string(m.value);
                                       buffer += " : %t";
                                       buffer += std::to_string(m.source_type);
                                       buffer += " -> %t";
                                       buffer += std::to_string(m.opaque_type);
                                       buffer += " id ";
                                       buffer += std::to_string(m.type_id);
                                       buffer += " canonical ";
                                       buffer += std::to_string(m.canonical_id.hi);
                                       buffer += ':';
                                       buffer += std::to_string(m.canonical_id.lo);
                                   },
                                   [&](const HirOpaqueCast &c) {
                                       buffer += "opaque_cast %e";
                                       buffer += std::to_string(c.value);
                                       buffer += " : %t";
                                       buffer += std::to_string(c.from);
                                       buffer += " -> %t";
                                       buffer += std::to_string(c.to);
                                       buffer += " id ";
                                       buffer += std::to_string(c.type_id);
                                       buffer += " canonical ";
                                       buffer += std::to_string(c.canonical_id.hi);
                                       buffer += ':';
                                       buffer += std::to_string(c.canonical_id.lo);
                                       buffer += c.checked ? " checked" : " raw";
                                   },
                                   [&](const HirOpaqueCheck &c) {
                                       buffer += "opaque_check %e";
                                       buffer += std::to_string(c.value);
                                       buffer += " : %t";
                                       buffer += std::to_string(c.opaque_type);
                                       buffer += " id ";
                                       buffer += std::to_string(c.type_id);
                                       buffer += " canonical ";
                                       buffer += std::to_string(c.canonical_id.hi);
                                       buffer += ':';
                                       buffer += std::to_string(c.canonical_id.lo);
                                   },
                                   [&](const HirRuntimePanic &p) {
                                       buffer += "runtime_panic R";
                                       buffer += std::to_string(p.code);
                                   },
                               });
                buffer += "\n";
            }
            if (block.terminator != kInvalidHirExpr) {
                auto &term = exprs_[block.terminator];
                hir::visitExpr(term, common::overloaded{
                                         [&](const HirRet &ret) {
                                             if (ret.value == kInvalidHirExpr)
                                                 buffer += "  ret void\n";
                                             else
                                                 buffer += "  ret %e";
                                             buffer += std::to_string(ret.value);
                                             buffer += "\n";
                                         },
                                         [&](const HirJump &jump) {
                                             buffer += "  jump bb";
                                             buffer += std::to_string(jump.target);
                                             buffer += "\n";
                                         },
                                         [&](const HirBranch &branch) {
                                             buffer += "  branch %e";
                                             buffer += std::to_string(branch.cond);
                                             buffer += " -> bb";
                                             buffer += std::to_string(branch.then_block);
                                             buffer += " : bb";
                                             buffer += std::to_string(branch.else_block);
                                             buffer += "\n";
                                         },
                                         [&](const HirStateTailCall &tail) {
                                             buffer += "  state_tail_call musttail -> ret %e";
                                             buffer += std::to_string(tail.call.args.size());
                                             buffer += "\n";
                                         },
                                         [&](const HirCleanup &cleanup) {
                                             buffer += "  cleanup reverse(";
                                             buffer += std::to_string(cleanup.exprs.size());
                                             buffer += ")\n";
                                         },
                                         [&](const HirRuntimePanic &panic) {
                                             buffer += "  runtime_panic R";
                                             buffer += std::to_string(panic.code);
                                             buffer += "\n";
                                         },
                                         [&](const auto &) {
                                             buffer += "  terminal %e";
                                             buffer += std::to_string(block.terminator);
                                             buffer += "\n";
                                         },
                                     });
            }
        }
        buffer += "}\n\n";
    }
    return buffer;
}

} // namespace zith::hir
