#include "hir-module.hpp"
#include "common/overloaded.hpp"
#include "hir/hir-expr.hpp"

#include <cstdio>
#include <string>

namespace zith::hir {

namespace {

void appendMarkerRetTerminator(std::string &buffer, const HirMarkerRet &ret) {
    buffer += "  marker_ret {";
    for (size_t i = 0; i < ret.continuations.size(); ++i) {
        if (i > 0)
            buffer += ", ";
        buffer += "bb";
        buffer += std::to_string(ret.continuations[i]);
    }
    buffer += "}\n";
}

} // namespace

HirModule::HirModule(memory::Arena &arena)
    : exprs_(arena), fns_(arena), marker_layout_(arena), attrs_(arena) {}

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

HirMarker &HirModule::addMarker() {
    marker_layout_.markers.emplace(exprs_.arena());
    return marker_layout_.markers.back();
}

const HirMarker &HirModule::getMarker(size_t idx) const {
    return marker_layout_.markers[idx];
}

HirMarker &HirModule::getMarkerMut(size_t idx) {
    return marker_layout_.markers[idx];
}

const HirMarker *HirModule::findMarker(memory::InternedId name) const {
    for (const auto &marker : marker_layout_.markers)
        if (marker.name == name)
            return &marker;
    return nullptr;
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
                                       if (jump.flowReturn) {
                                           buffer += " ret bb";
                                           buffer += std::to_string(jump.return_block);
                                       }
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
                                   [&](const HirUnionCast &c) {
                                       buffer += "union_cast %e";
                                       buffer += std::to_string(c.value);
                                       buffer += " : %t";
                                       buffer += std::to_string(c.from);
                                       buffer += " -> %t";
                                       buffer += std::to_string(c.to);
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
                                       buffer += i.which == HirLayoutIntrinsic::Which::OffsetOf
                                                     ? "offset_of"
                                                 : i.which == HirLayoutIntrinsic::Which::AlignOf
                                                     ? "align_of"
                                                     : "size_of";
                                       buffer += " %t";
                                       buffer += std::to_string(i.type);
                                   },
                                   [&](const HirMarkerStore &s) {
                                       buffer += "marker_store m";
                                       buffer += std::to_string(s.marker);
                                       buffer += "[";
                                       buffer += std::to_string(s.param_index);
                                       buffer += "] = %e";
                                       buffer += std::to_string(s.value);
                                   },
                                   [&](const HirMarkerLoad &s) {
                                       buffer += "marker_load m";
                                       buffer += std::to_string(s.marker);
                                       buffer += "[";
                                       buffer += std::to_string(s.param_index);
                                       buffer += "] : %t";
                                       buffer += std::to_string(s.type);
                                   },
                                   [&](const HirMarkerDock &d) {
                                       buffer += "marker_dock -> bb";
                                       buffer += std::to_string(d.marker_entry);
                                       buffer += " cont bb";
                                       buffer += std::to_string(d.continuation);
                                   },
                                   [&](const HirMarkerJump &j) {
                                       buffer += "marker_jump -> bb";
                                       buffer += std::to_string(j.marker_entry);
                                   },
                                   [&](const HirMarkerRet &) { buffer += "marker_ret"; },
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
                                             if (jump.flowReturn) {
                                                 buffer += " ret bb";
                                                 buffer += std::to_string(jump.return_block);
                                             }
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
                                         [&](const HirMarkerRet &ret) {
                                             appendMarkerRetTerminator(buffer, ret);
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
