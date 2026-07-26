#include "ast/ast-printer.hpp"
#include "common/overloaded.hpp"

namespace zith::ast {

namespace {

void print_indent(FILE *out, int depth) {
    for (int i = 0; i < depth; ++i)
        std::fprintf(out, "  ");
}

static const char *builtinName(BuiltinType k) {
    switch (k) {
    case BuiltinType::I8:
        return "i8";
    case BuiltinType::I16:
        return "i16";
    case BuiltinType::I32:
        return "i32";
    case BuiltinType::I64:
        return "i64";
    case BuiltinType::I128:
        return "i128";
    case BuiltinType::U8:
        return "u8";
    case BuiltinType::U16:
        return "u16";
    case BuiltinType::U32:
        return "u32";
    case BuiltinType::U64:
        return "u64";
    case BuiltinType::U128:
        return "u128";
    case BuiltinType::F32:
        return "f32";
    case BuiltinType::F64:
        return "f64";
    case BuiltinType::Bool:
        return "bool";
    case BuiltinType::Char:
        return "char";
    case BuiltinType::Void:
        return "void";
    case BuiltinType::Never:
        return "never";
    case BuiltinType::Unknown:
        return "unknown";
    case BuiltinType::Invalid:
        return "invalid";
    case BuiltinType::Opaque:
        return "opaque";
    }
    return "?";
}

static const char *ownershipName(OwnershipKw o) {
    switch (o) {
    case OwnershipKw::Default:
        return "";
    case OwnershipKw::Unique:
        return "unique ";
    case OwnershipKw::Share:
        return "share ";
    case OwnershipKw::Lend:
        return "lend ";
    case OwnershipKw::View:
        return "view ";
    case OwnershipKw::Belong:
        return "belong ";
    }
    return "?";
}

void print_type_expr(TypeExprId id, const AstBuilder &bld, FILE *out) {
    if (id == kInvalidTypeExpr) {
        std::fprintf(out, "<invalid>");
        return;
    }
    auto &node = bld.getTypeExpr(id);
    std::visit(common::overloaded{
                   [&](const TypePath &n) {
                       for (size_t i = 0; i < n.segments.size(); ++i) {
                           if (i > 0)
                               std::fprintf(out, "::");
                           std::fprintf(out, "%.*s", (int)n.segments[i].size(),
                                        n.segments[i].data());
                       }
                   },
                   [&](const TypeBuiltin &n) { std::fprintf(out, "%s", builtinName(n.kind)); },
                   [&](const TypePtrExpr &n) {
                       if (n.is_mut)
                           std::fprintf(out, "mut ");
                       std::fprintf(out, "%s", ownershipName(n.ownership));
                       print_type_expr(n.pointee, bld, out);
                   },
                   [&](const TypeSlice &n) {
                       std::fprintf(out, "[]");
                       print_type_expr(n.elem, bld, out);
                   },
                   [&](const TypeArray &n) {
                       std::fprintf(out, "[");
                       print_type_expr(n.count, bld, out);
                       std::fprintf(out, "]");
                       print_type_expr(n.elem, bld, out);
                   },
                   [&](const TypeFnExpr &n) {
                       std::fprintf(out, "(");
                       for (size_t i = 0; i < n.params.size(); ++i) {
                           if (i > 0)
                               std::fprintf(out, ", ");
                           print_type_expr(n.params[i], bld, out);
                       }
                       std::fprintf(out, ") -> ");
                       print_type_expr(n.ret, bld, out);
                   },
                   [&](const TypeOptional &n) {
                       auto &inner    = bld.getTypeExpr(n.inner);
                       bool needParen = std::holds_alternative<TypeFailable>(inner) ||
                                        std::holds_alternative<TypeSum>(inner);
                       if (needParen)
                           std::fprintf(out, "?(");
                       else
                           std::fprintf(out, "?");
                       print_type_expr(n.inner, bld, out);
                       if (needParen)
                           std::fprintf(out, ")");
                   },
                   [&](const TypeFailable &n) {
                       {
                           auto &inner     = bld.getTypeExpr(n.inner);
                           bool innerIsSum = std::holds_alternative<TypeSum>(inner);
                           if (innerIsSum)
                               std::fprintf(out, "(");
                           print_type_expr(n.inner, bld, out);
                           if (innerIsSum)
                               std::fprintf(out, ")");
                       }
                       std::fprintf(out, "!");
                       {
                           auto &error     = bld.getTypeExpr(n.error);
                           bool errorIsSum = std::holds_alternative<TypeSum>(error);
                           if (errorIsSum)
                               std::fprintf(out, "(");
                           print_type_expr(n.error, bld, out);
                           if (errorIsSum)
                               std::fprintf(out, ")");
                       }
                   },
                   [&](const TypeApp &n) {
                       print_type_expr(n.base, bld, out);
                       std::fprintf(out, "<");
                       for (size_t i = 0; i < n.args.size(); ++i) {
                           if (i > 0)
                               std::fprintf(out, ", ");
                           print_type_expr(n.args[i], bld, out);
                       }
                       std::fprintf(out, ">");
                   },
                   [&](const TypePack &) { std::fprintf(out, "{...}"); },
                   [&](const TypeSum &n) {
                       for (size_t i = 0; i < n.members.size(); ++i) {
                           if (i > 0)
                               std::fprintf(out, " | ");
                           print_type_expr(n.members[i], bld, out);
                       }
                   },
                   [&](const TypeInfer &) { std::fprintf(out, "_"); },
                   [&](const TypeMut &n) {
                       std::fprintf(out, "mut ");
                       print_type_expr(n.inner, bld, out);
                   },
                   [&](const TypeGenericParamRef &n) {
                       std::fprintf(out, "%.*s", (int)n.name.size(), n.name.data());
                   },
                   [&](const TypeDyn &n) {
                       std::fprintf(out, "dyn ");
                       print_type_expr(n.inner, bld, out);
                   },
                   [&](const TypeUnion &) { std::fprintf(out, "union"); },
                   [&](const TypeSpecialization &n) {
                       if (n.base != kInvalidTypeExpr) {
                           print_type_expr(n.base, bld, out);
                       }
                       std::fprintf(out, "<");
                       for (size_t i = 0; i < n.args.size(); ++i) {
                           if (i > 0)
                               std::fprintf(out, ", ");
                           print_type_expr(n.args[i], bld, out);
                       }
                       std::fprintf(out, ">");
                   },
               },
               node);
}

void print_stmt(StmtId id, const AstBuilder &bld, FILE *out, int depth);
void print_expr(ExprId id, const AstBuilder &bld, FILE *out, int depth);

void print_expr_node(const ExprNode &node, const AstBuilder &bld, FILE *out, int depth) {
    std::visit(
        common::overloaded{
            [&](const LitValue &n) {
                auto kind = [&]() -> const char * {
                    switch (n.kind) {
                    case LitKind::Int:
                        return "Int";
                    case LitKind::Float: {
                        auto sv = n.raw;
                        if (sv.size() >= 3 && sv.substr(sv.size() - 3) == "f32")
                            return "Flt";
                        return "Dbl";
                    }
                    case LitKind::Bool:
                        return "Bool";
                    case LitKind::String:
                        return "Str";
                    case LitKind::Char:
                        return "Char";
                    case LitKind::Nil:
                        return "Nil";
                    }
                    return "?";
                }();
                std::fprintf(out, "LitValue(%s, \"%.*s\", span=%u..%u)\n", kind, (int)n.raw.size(),
                             n.raw.data(), n.span.start, n.span.end);
            },
            [&](const IdentNode &n) {
                std::fprintf(out, "Ident(%.*s)\n", (int)n.name.size(), n.name.data());
            },
            [&](const BinaryNode &n) {
                const char *op = nullptr;
                switch (n.op) {
                case BinaryOp::Add:
                    op = "+";
                    break;
                case BinaryOp::Sub:
                    op = "-";
                    break;
                case BinaryOp::Mul:
                    op = "*";
                    break;
                case BinaryOp::Div:
                    op = "/";
                    break;
                case BinaryOp::Rest:
                    op = "%";
                    break;
                case BinaryOp::Eq:
                    op = "==";
                    break;
                case BinaryOp::Ne:
                    op = "!=";
                    break;
                case BinaryOp::Lt:
                    op = "<";
                    break;
                case BinaryOp::Le:
                    op = "<=";
                    break;
                case BinaryOp::Gt:
                    op = ">";
                    break;
                case BinaryOp::Ge:
                    op = ">=";
                    break;
                case BinaryOp::And:
                    op = "and";
                    break;
                case BinaryOp::Or:
                    op = "or";
                    break;
                case BinaryOp::Xor:
                    op = "^";
                    break;
                case BinaryOp::Shl:
                    op = "<<";
                    break;
                case BinaryOp::Shr:
                    op = ">>";
                    break;
                case BinaryOp::Is:
                    op = "is";
                    break;
                case BinaryOp::As:
                    op = "as";
                    break;
                }
                std::fprintf(out, "Binary(%s)\n", op);
                print_indent(out, depth + 1);
                print_expr(n.lhs, bld, out, depth + 1);
                print_indent(out, depth + 1);
                print_expr(n.rhs, bld, out, depth + 1);
            },
            [&](const UnaryNode &n) {
                const char *op = nullptr;
                switch (n.op) {
                case UnaryOp::Neg:
                    op = "-";
                    break;
                case UnaryOp::Not:
                    op = "not";
                    break;
                case UnaryOp::Ref:
                    op = "&";
                    break;
                case UnaryOp::Deref:
                    op = "*";
                    break;
                case UnaryOp::FallbackOpt:
                    op = "?";
                    break;
                case UnaryOp::FallbackRes:
                    op = "!";
                    break;
                case UnaryOp::PropagateOpt:
                    op = "?post";
                    break;
                case UnaryOp::PropagateRes:
                    op = "!post";
                    break;
                }
                std::fprintf(out, "Unary(%s)\n", op);
                print_indent(out, depth + 1);
                print_expr(n.operand, bld, out, depth + 1);
            },
            [&](const CallNode &n) {
                std::fprintf(out, "Call\n");
                print_indent(out, depth + 1);
                std::fprintf(out, "callee: ");
                print_expr(n.callee, bld, out, depth + 1);
                for (auto arg : n.args) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "arg: ");
                    print_expr(arg, bld, out, depth + 1);
                }
            },
            [&](const BlockNode &n) {
                std::fprintf(out, "Block\n");
                for (auto s : n.stmts) {
                    print_indent(out, depth + 1);
                    print_stmt(s, bld, out, depth + 1);
                }
                if (n.trailing != kInvalidExpr) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "trailing: ");
                    print_expr(n.trailing, bld, out, depth + 1);
                }
            },
            [&](const IfNode &n) {
                std::fprintf(out, "If\n");
                print_indent(out, depth + 1);
                std::fprintf(out, "cond: ");
                print_expr(n.cond, bld, out, depth + 1);
                print_indent(out, depth + 1);
                std::fprintf(out, "then: ");
                print_expr(n.then_branch, bld, out, depth + 1);
                if (n.else_branch != kInvalidExpr) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "else: ");
                    print_expr(n.else_branch, bld, out, depth + 1);
                }
            },
            [&](const WhileNode &n) {
                std::fprintf(out, "While\n");
                print_indent(out, depth + 1);
                std::fprintf(out, "cond: ");
                print_expr(n.cond, bld, out, depth + 1);
                print_indent(out, depth + 1);
                std::fprintf(out, "body: ");
                print_expr(n.body, bld, out, depth + 1);
            },
            [&](const FieldNode &n) {
                std::fprintf(out, "Field(%.*s)\n", (int)n.field.size(), n.field.data());
                print_indent(out, depth + 1);
                print_expr(n.object, bld, out, depth + 1);
            },
            [&](const IndexNode &n) {
                std::fprintf(out, "Index\n");
                print_indent(out, depth + 1);
                print_expr(n.object, bld, out, depth + 1);
                print_indent(out, depth + 1);
                print_expr(n.index, bld, out, depth + 1);
            },
            [&](const StructLiteralNode &n) {
                std::fprintf(out, "StructLiteral(%.*s)\n", static_cast<int>(n.type_name.size()),
                             n.type_name.data());
                for (const auto &field : n.fields) {
                    print_indent(out, depth + 1);
                    if (!field.name.empty()) {
                        if (field.value == kInvalidExpr) {
                            std::fprintf(out, "FieldInit %.*s: _\n",
                                         static_cast<int>(field.name.size()), field.name.data());
                        } else {
                            std::fprintf(out, "FieldInit %.*s:\n",
                                         static_cast<int>(field.name.size()), field.name.data());
                            print_indent(out, depth + 2);
                            print_expr(field.value, bld, out, depth + 2);
                        }
                    } else {
                        print_expr(field.value, bld, out, depth + 1);
                    }
                }
            },
            [&](const ArrayLiteralNode &n) {
                std::fprintf(out, "ArrayLiteral\n");
                for (auto elem : n.elements) {
                    print_indent(out, depth + 1);
                    print_expr(elem, bld, out, depth + 1);
                }
            },
            [&](const EnumValueNode &n) {
                std::fprintf(out, "EnumValue(%.*s.%.*s)\n", static_cast<int>(n.enum_name.size()),
                             n.enum_name.data(), static_cast<int>(n.variant_name.size()),
                             n.variant_name.data());
            },
            [&](const RangeNode &n) {
                const char *bound_str = "Closed";
                switch (n.bounds) {
                case RangeBounds::OpenLeft:  bound_str = "OpenLeft";  break;
                case RangeBounds::OpenRight: bound_str = "OpenRight"; break;
                case RangeBounds::Open:      bound_str = "Open";      break;
                default: break;
                }
                std::fprintf(out, "Range(%s)\n", bound_str);
                print_indent(out, depth + 1);
                print_expr(n.lhs, bld, out, depth + 1);
                print_indent(out, depth + 1);
                print_expr(n.rhs, bld, out, depth + 1);
            },
            [&](const UnbodyNode &n) {
                std::fprintf(out, "Unbody(spans [%u..%u], tokens [%u..%u])\n", n.body_span.start,
                             n.body_span.end, n.token_start, n.token_end);
            },
            [&](const IntrinsicNode &n) {
                std::fprintf(out, "Intrinsic(%d)\n", static_cast<int>(n.kind));
                for (auto arg : n.args) {
                    print_indent(out, depth + 1);
                    print_expr(arg, bld, out, depth + 1);
                }
            },
            [&](const MacroCallNode &n) {
                std::fprintf(out, "MacroCall(%.*s)\n", (int)n.name.size(), n.name.data());
                for (auto arg : n.args) {
                    print_indent(out, depth + 1);
                    print_expr(arg, bld, out, depth + 1);
                }
            },
            [&](const SeqNode &n) {
                std::fprintf(out, "Seq\n");
                for (size_t i = 0; i < n.operands.size(); ++i) {
                    print_indent(out, depth + 1);
                    print_expr(n.operands[i], bld, out, depth + 1);
                    if (i < n.ops.size()) {
                        auto &op = n.ops[i];
                        if (op.is_word) {
                            std::fprintf(out, "  word(%.*s)\n", (int)op.word_name.size(),
                                         op.word_name.data());
                        } else {
                            std::fprintf(out, "  op(%d)\n", static_cast<int>(op.builtin_op));
                        }
                    }
                }
            },
            [&](const WhenNode &n) {
                std::fprintf(out, "When\n");
                print_indent(out, depth + 1);
                std::fprintf(out, "subject:\n");
                print_indent(out, depth + 2);
                print_expr(n.subject, bld, out, depth + 2);
                for (size_t i = 0; i < n.cases.size(); i++) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "case %zu (%s):\n", i, n.cases[i].is_default ? "default" : "pattern");
                    print_indent(out, depth + 2);
                    std::fprintf(out, "condition:\n");
                    print_indent(out, depth + 3);
                    print_expr(n.cases[i].condition, bld, out, depth + 3);
                    print_indent(out, depth + 2);
                    std::fprintf(out, "body:\n");
                    print_indent(out, depth + 3);
                    print_expr(n.cases[i].body, bld, out, depth + 3);
                }
            },
            [&](const WordCallNode &n) {
                std::fprintf(out, "WordCall(%.*s)\n", (int)n.word_name.size(), n.word_name.data());
                for (auto arg : n.args) {
                    print_indent(out, depth + 1);
                    print_expr(arg, bld, out, depth + 1);
                }
            },
            [&](const ErrorExprNode &) { std::fprintf(out, "<error-expr>\n"); },
        },
        node);
}

void print_expr(ExprId id, const AstBuilder &bld, FILE *out, int depth) {
    if (id == kInvalidExpr) {
        std::fprintf(out, "<invalid>\n");
        return;
    }
    auto &node = bld.getExpr(id);
    print_expr_node(node, bld, out, depth);
}

void print_stmt(StmtId id, const AstBuilder &bld, FILE *out, int depth) {
    if (id == kInvalidStmt) {
        std::fprintf(out, "<invalid stmt>\n");
        return;
    }
    auto &node = bld.getStmt(id);
    std::visit(common::overloaded{
                   [&](const LetNode &n) {
                       auto var_name = n.names.empty() ? std::string_view{} : n.names[0];
                       std::fprintf(out, "Let(%.*s, mut=%s)\n", (int)var_name.size(),
                                    var_name.data(), n.mut ? "true" : "false");
                       if (n.init != kInvalidExpr) {
                           print_indent(out, depth + 1);
                           std::fprintf(out, "init: ");
                           print_expr(n.init, bld, out, depth + 1);
                       }
                   },
                   [&](const AssignNode &n) {
                       std::fprintf(out, "Assign\n");
                       print_indent(out, depth + 1);
                       print_expr(n.target, bld, out, depth + 1);
                       print_indent(out, depth + 1);
                       print_expr(n.value, bld, out, depth + 1);
                   },
                   [&](const RetNode &n) {
                       std::fprintf(out, "Return\n");
                       if (n.value != kInvalidExpr) {
                           print_indent(out, depth + 1);
                           print_expr(n.value, bld, out, depth + 1);
                       }
                   },
                   [&](const GotoNode &n) {
                       std::fprintf(out, "Goto(%.*s)\n", (int)n.target.size(), n.target.data());
                   },
                   [&](const MarkerNode &n) {
                       std::fprintf(out, "Marker(%.*s)\n", (int)n.name.size(), n.name.data());
                   },
                   [&](const ExprStmtNode &n) { print_expr(n.expr, bld, out, depth); },
                   [&](const UseNode &n) {
                       std::fprintf(out, "Use(context=%.*s, alias=%.*s, target=%.*s)\n",
                                    (int)n.context_name.size(), n.context_name.data(),
                                    (int)n.alias_name.size(), n.alias_name.data(),
                                    (int)n.target_path.size(), n.target_path.data());
                       for (auto w : n.words) {
                           print_indent(out, depth + 1);
                           std::fprintf(out, "word: %.*s\n", (int)w.size(), w.data());
                       }
                       if (n.block != kInvalidExpr) {
                           print_indent(out, depth + 1);
                           print_expr(n.block, bld, out, depth + 1);
                       }
                   },
                   [&](const ErrorStmtNode &) { std::fprintf(out, "<error-stmt>\n"); },
               },
               node);
}

void print_decl(DeclId id, const AstBuilder &bld, FILE *out, int depth) {
    if (id == kInvalidDecl) {
        std::fprintf(out, "<invalid decl>\n");
        return;
    }
    auto &node = bld.getDecl(id);
    std::visit(
        common::overloaded{
            [&](const FnDeclNode &n) {
                std::fprintf(out, "Fn(%s%.*s)\n", n.is_extern ? "extern " : "", (int)n.name.size(),
                             n.name.data());
                if (n.params.size() > 0) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "params: ");
                    for (size_t i = 0; i < n.params.size(); ++i) {
                        if (i > 0)
                            std::fprintf(out, ", ");
                        std::fprintf(out, "%.*s", (int)n.params[i].name.size(),
                                     n.params[i].name.data());
                    }
                    std::fprintf(out, "\n");
                }
                if (n.body != kInvalidExpr) {
                    print_indent(out, depth + 1);
                    print_expr(n.body, bld, out, depth + 1);
                }
            },
            [&](const WordDeclNode &n) {
                const char *cat_str = "Nop";
                if (n.category == WordCategory::Prefix)
                    cat_str = "Prefix";
                else if (n.category == WordCategory::Suffix)
                    cat_str = "Suffix";
                else if (n.category == WordCategory::Infix)
                    cat_str = "Infix";
                std::fprintf(out, "WordDecl(%.*s, category=%s)\n", (int)n.name.size(),
                             n.name.data(), cat_str);
                for (auto p : n.params) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "param: %.*s\n", (int)p.size(), p.data());
                }
                if (n.body != kInvalidExpr) {
                    print_indent(out, depth + 1);
                    print_expr(n.body, bld, out, depth + 1);
                }
            },
            [&](const ContextDeclNode &n) {
                std::fprintf(out, "ContextDecl(%.*s)\n", (int)n.name.size(), n.name.data());
                for (auto d : n.decls) {
                    print_indent(out, depth + 1);
                    print_decl(d, bld, out, depth + 1);
                }
                if (n.body != kInvalidExpr) {
                    print_indent(out, depth + 1);
                    print_expr(n.body, bld, out, depth + 1);
                }
            },
            [&](const StructDeclNode &n) {
                std::fprintf(out, "Struct(%.*s)\n", (int)n.name.size(), n.name.data());
                for (auto &gp : n.generic_params) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "generic %.*s\n", (int)gp.name.size(), gp.name.data());
                }
                if (n.extends_parent != kInvalidTypeExpr) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "extends\n");
                }
                for ([[maybe_unused]] auto &t : n.traits) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "trait\n");
                }
                for (auto &f : n.fields) {
                    print_indent(out, depth + 1);
                    // qualifier
                    const char *bname = "";
                    switch (f.bind) {
                    case FieldBind::Auto:
                        bname = "";
                        break;
                    case FieldBind::Const:
                        bname = "const ";
                        break;
                    case FieldBind::Let:
                        bname = "let ";
                        break;
                    case FieldBind::Var:
                        bname = "var ";
                        break;
                    case FieldBind::Global:
                        bname = "global ";
                        break;
                    case FieldBind::Comptime:
                        bname = "comptime ";
                        break;
                    }
                    std::fprintf(out, "field %s%.*s", bname, (int)f.name.size(), f.name.data());
                    if (f.type_expr != kInvalidTypeExpr) {
                        std::fprintf(out, ": ");
                        print_type_expr(f.type_expr, bld, out);
                    }
                    if (f.default_value != kInvalidExpr) {
                        std::fprintf(out, " = ");
                        print_expr(f.default_value, bld, out, 0);
                    }
                    std::fprintf(out, "\n");
                }
                for (auto &m : n.methods) {
                    auto &decl = bld.getDecl(m);
                    if (auto *fn = std::get_if<FnDeclNode>(&decl)) {
                        print_indent(out, depth + 1);
                        std::fprintf(out, "method %.*s(", (int)fn->name.size(), fn->name.data());
                        for (size_t i = 0; i < fn->params.size(); ++i) {
                            if (i > 0)
                                std::fprintf(out, ", ");
                            std::fprintf(out, "%.*s", (int)fn->params[i].name.size(),
                                         fn->params[i].name.data());
                        }
                        std::fprintf(out, ")");
                        if (fn->return_type != kInvalidTypeExpr) {
                            std::fprintf(out, ": ");
                            print_type_expr(fn->return_type, bld, out);
                        }
                        std::fprintf(out, "\n");
                        if (fn->body != kInvalidExpr) {
                            print_indent(out, depth + 1);
                            print_expr(fn->body, bld, out, depth + 1);
                        }
                    }
                }
            },
            [&](const EnumDeclNode &n) {
                std::fprintf(out, "Enum(%.*s)\n", (int)n.name.size(), n.name.data());
                for (auto &gp : n.generic_params) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "generic %.*s\n", (int)gp.name.size(), gp.name.data());
                }
                for (auto &v : n.variants) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "variant %.*s\n", (int)v.name.size(), v.name.data());
                }
            },
            [&](const UnionDeclNode &n) {
                std::fprintf(out, "Union(%.*s)\n", (int)n.name.size(), n.name.data());
                for (auto &gp : n.generic_params) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "generic %.*s\n", (int)gp.name.size(), gp.name.data());
                }
                for (auto &v : n.variants) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "variant %.*s\n", (int)v.name.size(), v.name.data());
                }
            },
            [&](const ComponentDeclNode &n) {
                std::fprintf(out, "Component(%.*s)\n", (int)n.name.size(), n.name.data());
                for (auto &gp : n.generic_params) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "generic %.*s\n", (int)gp.name.size(), gp.name.data());
                }
            },
            [&](const TraitDeclNode &n) {
                std::fprintf(out, "Trait(%.*s)\n", (int)n.name.size(), n.name.data());
                for (auto &gp : n.generic_params) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "generic %.*s\n", (int)gp.name.size(), gp.name.data());
                }
                for (auto &m : n.methods) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "method %.*s\n", (int)m.name.size(), m.name.data());
                }
            },
            [&](const InterfaceDeclNode &n) {
                std::fprintf(out, "Interface(%.*s)\n", (int)n.name.size(), n.name.data());
                for (auto &gp : n.generic_params) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "generic %.*s\n", (int)gp.name.size(), gp.name.data());
                }
                for (auto &m : n.methods) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "method %.*s\n", (int)m.name.size(), m.name.data());
                }
            },
            [&](const ImportNode &n) {
                std::fprintf(out, "%s%s(", n.is_export ? "Export " : "",
                             n.is_from ? "From" : "Import");
                for (size_t i = 0; i < n.path.size(); ++i) {
                    if (i > 0)
                        std::fprintf(out, "/");
                    std::fprintf(out, "%.*s", (int)n.path[i].size(), n.path[i].data());
                }
                if (n.import_depth == -1)
                    std::fprintf(out, "(..)");
                else if (n.import_depth > 1)
                    std::fprintf(out, "(%d)", n.import_depth);
                if (!n.symbols.empty()) {
                    std::fprintf(out, " {");
                    for (size_t i = 0; i < n.symbols.size(); ++i) {
                        if (i > 0)
                            std::fprintf(out, ",");
                        auto &s = n.symbols[i];
                        std::fprintf(out, " %.*s", (int)s.name.size(), s.name.data());
                        if (!s.alias.empty())
                            std::fprintf(out, " as %.*s", (int)s.alias.size(), s.alias.data());
                    }
                    std::fprintf(out, " }");
                }
                if (!n.alias.empty())
                    std::fprintf(out, " as %.*s", (int)n.alias.size(), n.alias.data());
                if (n.is_asset)
                    std::fprintf(out, " [asset]");
                std::fprintf(out, ")\n");
            },
            [&](const TypeAliasDeclNode &n) {
                std::fprintf(out, "Alias(%.*s", (int)n.name.size(), n.name.data());
                if (n.target_type != kInvalidTypeExpr) {
                    std::fprintf(out, " = ");
                    print_type_expr(n.target_type, bld, out);
                }
                std::fprintf(out, ")\n");
                for (auto &gp : n.generic_params) {
                    print_indent(out, depth + 1);
                    std::fprintf(out, "generic %.*s\n", (int)gp.name.size(), gp.name.data());
                }
            },
            [&](const GlobalDeclNode &n) {
                std::fprintf(out, "%s %.*s", n.is_const ? "Const" : "Global", (int)n.name.size(),
                             n.name.data());
                if (n.type_annot != kInvalidTypeExpr) {
                    std::fprintf(out, ": ");
                    print_type_expr(n.type_annot, bld, out);
                }
                if (n.init != kInvalidExpr) {
                    std::fprintf(out, " = ");
                    print_expr(n.init, bld, out, 0);
                }
                std::fprintf(out, "\n");
            },
            [&](const ErrorDeclNode &) { std::fprintf(out, "<error-decl>\n"); },
        },
        node);
}

} // anonymous namespace

static bool isStructMethod(DeclId id, const AstBuilder &bld, const ProgramNode &program) {
    for (auto d : program.decls) {
        auto &node = bld.getDecl(d);
        if (auto *sn = std::get_if<StructDeclNode>(&node))
            for (auto m : sn->methods)
                if (m == id)
                    return true;
    }
    return false;
}

void printAST(const ProgramNode &program, const AstBuilder &builder, FILE *out) {
    std::fprintf(out, "Program\n");
    for (auto decl : program.decls) {
        // skip FnDeclNodes that belong to a struct (printed inside the Struct node)
        if (std::get_if<FnDeclNode>(&builder.getDecl(decl)))
            if (isStructMethod(decl, builder, program))
                continue;
        std::fprintf(out, "  ");
        print_decl(decl, builder, out, 1);
    }
    std::fprintf(out, "End\n");
    std::fflush(out);
}

} // namespace zith::ast
