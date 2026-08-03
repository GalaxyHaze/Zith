#include "hir-module.hpp"
#include "common/overloaded.hpp"

#include <cstdio>

namespace zith::hir {

HirModule::HirModule(memory::Arena &arena) : exprs_(arena), fns_(arena) {}

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

size_t HirModule::getFnCount() const {
    return fns_.size();
}

void HirModule::dump(FILE *out, const memory::StringInterner &interner) const {
    for (size_t fi = 0; fi < fns_.size(); ++fi) {
        auto &fn     = fns_[fi];
        auto fn_name = interner.lookup(fn.name);
        if (fn.blocks.empty()) {
            std::fprintf(out, "fn %.*s : extern\n", (int)fn_name.size(), fn_name.data());
            continue;
        }
        std::fprintf(out, "fn %.*s", (int)fn_name.size(), fn_name.data());
        std::fprintf(out, "(");
        for (size_t pi = 0; pi < fn.params.size(); ++pi) {
            if (pi > 0)
                std::fprintf(out, ", ");
            std::fprintf(out, "%%p%zu", pi);
        }
        std::fprintf(out, ")");
        std::fprintf(out, " -> %%t%u", fn.return_type);
        std::fprintf(out, " {\n");
        for (auto &block : fn.blocks) {
            for (auto inst_id : block.insts) {
                std::fprintf(out, "  %%e%u = ", inst_id);
                auto &expr = exprs_[inst_id];
                hir::visitExpr(
                    expr,
                    common::overloaded{
                        [&](const HirLiteral &lit) {
                            if (lit.type == static_cast<HirTypeId>(-1))
                                std::fprintf(out, "literal<?>");
                            else
                                std::fprintf(out, "literal %%t%u", lit.type);
                        },
                        [&](const HirBinary &bin) {
                            std::fprintf(out, "binary %%e%u op %%e%u", bin.lhs, bin.rhs);
                        },
                        [&](const HirUnary &un) {
                            std::fprintf(out, "unary %%t%u op %%e%u", un.type, un.operand);
                        },
                        [&](const HirLet &let) {
                            auto n = interner.lookup(let.name);
                            std::fprintf(out, "let %.*s", (int)n.size(), n.data());
                        },
                        [&](const HirVar &var) {
                            auto n = interner.lookup(var.name);
                            std::fprintf(out, "var %.*s#%u", (int)n.size(), n.data(), var.version);
                        },
                        [&](const HirCall &call) {
                            auto &callee = exprs_[call.callee];
                            if (auto *calleeVar = std::get_if<HirVar>(&callee)) {
                                auto n = interner.lookup(calleeVar->name);
                                std::fprintf(out, "call %.*s(", (int)n.size(), n.data());
                            } else {
                                std::fprintf(out, "call %%e%u(", call.callee);
                            }
                            for (size_t ai = 0; ai < call.args.size(); ++ai) {
                                if (ai > 0)
                                    std::fprintf(out, ", ");
                                std::fprintf(out, "%%e%u", call.args[ai]);
                            }
                            std::fprintf(out, ")");
                        },
                        [&](const HirRet &ret) {
                            if (ret.value == kInvalidHirExpr)
                                std::fprintf(out, "ret void");
                            else
                                std::fprintf(out, "ret %%e%u", ret.value);
                        },
                        [&](const HirJump &jump) { std::fprintf(out, "jump bb%u", jump.target); },
                        [&](const HirBranch &branch) {
                            std::fprintf(out, "branch %%e%u -> bb%u : bb%u", branch.cond,
                                         branch.then_block, branch.else_block);
                        },
                        [&](const HirPhi &) { std::fprintf(out, "<expr>"); },
                        [&](const HirAssign &assign) {
                            std::fprintf(out, "assign %%e%u = %%e%u", assign.target, assign.value);
                        },
                        [&](const HirIndex &idx) {
                            std::fprintf(out, "index %%e%u[%%e%u]", idx.object, idx.index);
                        },
                        [&](const HirField &field) {
                            std::fprintf(out, "field %%e%u.%u", field.object, field.index);
                        },
                        [&](const HirStructLiteral &literal) {
                            std::fprintf(out, "struct_literal %%t%u", literal.type);
                        },
                        [&](const HirArrayLiteral &literal) {
                            std::fprintf(out, "array_literal %%t%u", literal.type);
                        },
                        [&](const HirEnumValue &value) {
                            std::fprintf(out, "enum_value %%t%u", value.type);
                        },
                        [&](const HirSlotAlloca &s) {
                            std::fprintf(out, "slot_alloca s%u : %%t%u", s.slot, s.type);
                        },
                        [&](const HirSlotStore &s) {
                            std::fprintf(out, "slot_store s%u = %%e%u", s.slot, s.value);
                        },
                        [&](const HirSlotLoad &s) {
                            std::fprintf(out, "slot_load s%u : %%t%u", s.slot, s.type);
                        },
                        [&](const HirSlotAddr &s) {
                            std::fprintf(out, "slot_addr s%u : %%t%u", s.slot, s.type);
                        },
                        [&](const HirMakeNone &m) {
                            std::fprintf(out, "make_none : %%t%u", m.type);
                        },
                        [&](const HirMakeSome &m) {
                            std::fprintf(out, "make_some %%e%u : %%t%u", m.value, m.type);
                        },
                        [&](const HirCast &c) {
                            std::fprintf(out, "cast %%e%u : %%t%u -> %%t%u", c.value, c.from, c.to);
                        },
                        [&](const HirLayoutIntrinsic &i) {
                            std::fprintf(out, "%s %%t%u",
                                         i.which == HirLayoutIntrinsic::Which::OffsetOf
                                             ? "offset_of"
                                             : "align_of",
                                         i.type);
                        },
                    });
                std::fprintf(out, "\n");
            }
            if (block.terminator != kInvalidHirExpr) {
                auto &term = exprs_[block.terminator];
                hir::visitExpr(
                    term, common::overloaded{
                              [&](const HirRet &ret) {
                                  if (ret.value == kInvalidHirExpr)
                                      std::fprintf(out, "  ret void\n");
                                  else
                                      std::fprintf(out, "  ret %%e%u\n", ret.value);
                              },
                              [&](const HirJump &jump) {
                                  std::fprintf(out, "  jump bb%u\n", jump.target);
                              },
                              [&](const HirBranch &branch) {
                                  std::fprintf(out, "  branch %%e%u -> bb%u : bb%u\n", branch.cond,
                                               branch.then_block, branch.else_block);
                              },
                              [&](const auto &) {
                                  std::fprintf(out, "  terminal %%e%u\n", block.terminator);
                              },
                          });
            }
        }
        std::fprintf(out, "}\n\n");
    }
}

} // namespace zith::hir
