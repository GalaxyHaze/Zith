// test-parser-expr.cpp - Parser array indexing and custom words tests
//
// Atua como um Engenheiro de Testes de Software Especialista em Compiladores.
// Garante cobertura exaustiva de testes para indexação de arrays e sequências de word operators.

#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/error-codes.hpp"
#include "legacy-zith/ast/ast-builder.hpp"
#include "legacy-zith/ast/ast-node-utils.hpp"
#include "legacy-zith/ast/ast-nodes.hpp"
#include "legacy-zith/lexer/lexer.hpp"
#include "legacy-zith/parser/parser.hpp"
#include "memory/arena.hpp"
#include "memory/source-map.hpp"
#include "memory/string-interner.hpp"
#include "test-common.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <variant>

using namespace zith;

struct ExprParserTest {
    memory::Arena arena;
    memory::StringInterner interner;
    memory::SourceMap sourceMap;
    diagnostics::DiagnosticEngine diags;
    lexer::TokenStream tokens;

    ExprParserTest() : interner(arena), diags(arena), tokens{} {}

    struct Result {
        ast::AstBuilder *builder                 = nullptr;
        ast::ExprId exprId                       = ast::kInvalidExpr;
        bool ok                                  = false;
        size_t errorCount                        = 0;
        diagnostics::DiagnosticEngine *diags_ptr = nullptr;
    };

    Result parse(std::string_view input) {
        auto addResult = sourceMap.addFile("test.zith", input);
        if (!addResult)
            return {nullptr, ast::kInvalidExpr, false, 1, &diags};
        auto fileId      = addResult.value();
        auto tokenResult = lexer::tokenize(sourceMap, arena, fileId, diags);
        if (!tokenResult) {
            size_t errs = 0;
            for (auto &d : diags.all())
                if (d.severity == diagnostics::Severity::Error)
                    errs++;
            return {nullptr, ast::kInvalidExpr, false, errs, &diags};
        }
        tokens        = std::move(tokenResult.value());
        auto *builder = arena.make<ast::AstBuilder>(arena, interner);
        parser::Parser parser(&tokens, builder, &diags);
        auto exprId = parser.parseExpr();

        size_t errs = 0;
        for (auto &d : diags.all())
            if (d.severity == diagnostics::Severity::Error)
                errs++;
        return {builder, exprId, errs == 0, errs, &diags};
    }
};

// ── A. Caminhos Felizes (Happy Paths) ───────────────────────────────────

static void test_happy_paths() {
    // Caso A1: Indexação de array simples
    {
        ExprParserTest t;
        auto r = t.parse("a[0]");
        CHECK(r.ok, "Caso A1: parse bem-sucedido de indexacao simples");
        CHECK(r.exprId != ast::kInvalidExpr, "Caso A1: ExprId valido retornado");
        auto *idx = std::get_if<ast::IndexNode>(&r.builder->getExpr(r.exprId));
        CHECK(idx != nullptr, "Caso A1: AST contem um IndexNode");
        if (idx) {
            auto *obj = std::get_if<ast::IdentNode>(&r.builder->getExpr(idx->object));
            CHECK(obj != nullptr && obj->name == "a", "Caso A1: objeto indexado e 'a'");
            auto *index_val = std::get_if<ast::LitValue>(&r.builder->getExpr(idx->index));
            CHECK(index_val != nullptr && index_val->raw == "0", "Caso A1: indice e literal '0'");
        }
    }

    // Caso A2: Operador customizado simples
    {
        ExprParserTest t;
        auto r = t.parse("a nop b");
        CHECK(r.ok, "Caso A2: parse bem-sucedido de word operator simples");
        auto *seq = std::get_if<ast::SeqNode>(&r.builder->getExpr(r.exprId));
        CHECK(seq != nullptr, "Caso A2: AST contem um SeqNode");
        if (seq) {
            CHECK_EQ(seq->operands.size(), 2, "Caso A2: possui 2 operandos");
            CHECK_EQ(seq->ops.size(), 1, "Caso A2: possui 1 operador");
            CHECK(seq->ops[0].is_word && seq->ops[0].word_name == "nop",
                  "Caso A2: operador e 'nop'");
        }
    }

    // Caso A3: Regressao de sequencia de nops (garante que b nao e omitido)
    {
        ExprParserTest t;
        auto r = t.parse("a nop b nop c");
        CHECK(r.ok, "Caso A3: parse bem-sucedido de sequencia de nops");
        auto *seq = std::get_if<ast::SeqNode>(&r.builder->getExpr(r.exprId));
        CHECK(seq != nullptr, "Caso A3: AST contem SeqNode valido");
        if (seq) {
            CHECK_EQ(seq->operands.size(), 3, "Caso A3: possui exatamente 3 operandos");
            CHECK_EQ(seq->ops.size(), 2, "Caso A3: possui exatamente 2 operadores");
            auto *op1 = std::get_if<ast::IdentNode>(&r.builder->getExpr(seq->operands[0]));
            auto *op2 = std::get_if<ast::IdentNode>(&r.builder->getExpr(seq->operands[1]));
            auto *op3 = std::get_if<ast::IdentNode>(&r.builder->getExpr(seq->operands[2]));
            CHECK(op1 && op1->name == "a", "Caso A3: operando 1 e 'a'");
            CHECK(op2 && op2->name == "b", "Caso A3: operando 2 e 'b'");
            CHECK(op3 && op3->name == "c", "Caso A3: operando 3 e 'c'");
        }
    }

    // Caso A4: Precedencia nativa misturada com operador customizado
    {
        ExprParserTest t;
        auto r = t.parse("a + b nop c * d");
        CHECK(r.ok, "Caso A4: parse bem-sucedido de precedencia misturada");
        auto *seq = std::get_if<ast::SeqNode>(&r.builder->getExpr(r.exprId));
        CHECK(seq != nullptr, "Caso A4: AST contem SeqNode");
        if (seq) {
            CHECK_EQ(seq->operands.size(), 2, "Caso A4: possui 2 operandos na sequencia principal");
            auto *lhs_bin = std::get_if<ast::BinaryNode>(&r.builder->getExpr(seq->operands[0]));
            auto *rhs_bin = std::get_if<ast::BinaryNode>(&r.builder->getExpr(seq->operands[1]));
            CHECK(lhs_bin && lhs_bin->op == ast::BinaryOp::Add,
                  "Caso A4: operando esquerdo e 'a + b'");
            CHECK(rhs_bin && rhs_bin->op == ast::BinaryOp::Mul,
                  "Caso A4: operando direito e 'c * d'");
        }
    }
}

// ── B. Casos de Fronteira e Limites (Edge Cases) ───────────────────────

static void test_boundary_cases() {
    // Caso B1: Entrada vazia
    {
        ExprParserTest t;
        auto r = t.parse("");
        CHECK(!r.ok, "Caso B1: entrada vazia deve falhar");
        bool found_expected_error = false;
        for (auto &d : r.diags_ptr->all()) {
            if (d.message.find("expected expression but got End") != std::string::npos) {
                found_expected_error = true;
            }
        }
        CHECK(found_expected_error, "Caso B1: gera mensagem 'expected expression but got End'");
    }

    // Caso B2: Operador customizado sem operando a esquerda
    {
        ExprParserTest t;
        auto r = t.parse("nop b");
        CHECK(!r.ok, "Caso B2: nop sem lhs deve falhar");
        bool found_expected_error = false;
        for (auto &d : r.diags_ptr->all()) {
            if (d.message.find("expected expression but got Word") != std::string::npos) {
                found_expected_error = true;
            }
        }
        CHECK(found_expected_error, "Caso B2: gera mensagem 'expected expression but got Word'");
    }

    // Caso B3: Operador customizado sem operando a direita
    {
        ExprParserTest t;
        auto r = t.parse("a nop");
        CHECK(!r.ok, "Caso B3: nop sem rhs deve falhar");
        bool found_expected_error = false;
        for (auto &d : r.diags_ptr->all()) {
            if (d.message.find("expected expression but got End") != std::string::npos) {
                found_expected_error = true;
            }
        }
        CHECK(found_expected_error, "Caso B3: gera mensagem 'expected expression but got End'");
    }

    // Caso B4: Identificador extremamente longo como operando de indexacao
    {
        std::string long_ident(5000, 'x');
        std::string input = long_ident + "[0]";
        ExprParserTest t;
        auto r = t.parse(input);
        CHECK(r.ok, "Caso B4: parse de identificador longo com indexacao");
        auto *idx = std::get_if<ast::IndexNode>(&r.builder->getExpr(r.exprId));
        CHECK(idx != nullptr, "Caso B4: AST contem um IndexNode");
        if (idx) {
            auto *obj = std::get_if<ast::IdentNode>(&r.builder->getExpr(idx->object));
            CHECK(obj != nullptr && obj->name.size() == 5000,
                  "Caso B4: identificador longo recuperado");
        }
    }

    // Caso B5: Cadeia massiva de operadores customizados (100 nops)
    {
        std::string input = "a";
        for (int i = 0; i < 100; ++i) {
            input += " nop a";
        }
        ExprParserTest t;
        auto r = t.parse(input);
        CHECK(r.ok, "Caso B5: parse de cadeia massiva de nops");
        auto *seq = std::get_if<ast::SeqNode>(&r.builder->getExpr(r.exprId));
        CHECK(seq != nullptr, "Caso B5: AST contem SeqNode");
        if (seq) {
            CHECK_EQ(seq->operands.size(), 101, "Caso B5: possui exatamente 101 operandos");
            CHECK_EQ(seq->ops.size(), 100, "Caso B5: possui exatamente 100 nops");
        }
    }

    // Caso B6: Indexacoes de array profundamente aninhadas
    {
        ExprParserTest t;
        auto r = t.parse("a[b[c[d[0]]]]");
        CHECK(r.ok, "Caso B6: parse de indexacao profundamente aninhada");
        auto *idx1 = std::get_if<ast::IndexNode>(&r.builder->getExpr(r.exprId));
        CHECK(idx1 != nullptr, "Caso B6: nivel 1 e IndexNode");
        if (idx1) {
            auto *idx2 = std::get_if<ast::IndexNode>(&r.builder->getExpr(idx1->index));
            CHECK(idx2 != nullptr, "Caso B6: nivel 2 e IndexNode");
            if (idx2) {
                auto *idx3 = std::get_if<ast::IndexNode>(&r.builder->getExpr(idx2->index));
                CHECK(idx3 != nullptr, "Caso B6: nivel 3 e IndexNode");
            }
        }
    }
}

// ── C. Tratamento de Erros e Resiliencia (Errors & Resilience) ────────

static void test_error_handling() {
    // Caso C1: Conflito de operadores customizados ambiguos sem parenteses
    {
        ExprParserTest t;
        auto r = t.parse("a prefix b infix c");
        CHECK(!r.ok, "Caso C1: conflito de word operators sem parenteses deve falhar");
        bool found_ambiguity_msg = false;
        for (auto &d : r.diags_ptr->all()) {
            if (d.message.find("ambiguous word operators; use parentheses") != std::string::npos) {
                found_ambiguity_msg = true;
            }
        }
        CHECK(found_ambiguity_msg, "Caso C1: gera erro de ambiguidade apropriado");
    }

    // Caso C2: Tres operadores adjacentes nao-nop
    {
        ExprParserTest t;
        auto r = t.parse("a prefix b suffix c infix d");
        CHECK(!r.ok, "Caso C2: multiplos operadores adjacentes sem parenteses deve falhar");
        size_t ambiguity_errors = 0;
        for (auto &d : r.diags_ptr->all()) {
            if (d.message.find("ambiguous word operators; use parentheses") != std::string::npos) {
                ambiguity_errors++;
            }
        }
        CHECK(ambiguity_errors >= 1, "Caso C2: gera pelo menos 1 erro de ambiguidade");
    }

    // Caso C3: Parenteses fechados incorretamente em indexacao
    {
        ExprParserTest t;
        auto r = t.parse("a[0");
        CHECK(!r.ok, "Caso C3: indexacao inacabada deve falhar");
        bool found_unclosed_err = false;
        for (auto &d : r.diags_ptr->all()) {
            if (d.message.find("expected ']' but got") != std::string::npos) {
                found_unclosed_err = true;
            }
        }
        CHECK(found_unclosed_err, "Caso C3: gera erro esperado para parentese/colchete unclosed");
    }

    // Caso C4: Indexacao sem expressao interna
    {
        ExprParserTest t;
        auto r = t.parse("a[]");
        CHECK(!r.ok, "Caso C4: indexacao vazia deve falhar");
        bool found_expr_err = false;
        for (auto &d : r.diags_ptr->all()) {
            if (d.message.find("expected expression but got Punctuation") != std::string::npos) {
                found_expr_err = true;
            }
        }
        CHECK(found_expr_err, "Caso C4: gera erro 'expected expression'");
    }
}

// ── D. Casos Complexos ou Combinatorios ───────────────────────────────

static void test_complex_combinatorial() {
    // Caso D1: Resolucao de ambiguidade com parenteses
    {
        ExprParserTest t;
        auto r = t.parse("(a prefix b) infix c");
        CHECK(r.ok, "Caso D1: parse com parenteses resolvendo a ambiguidade a esquerda");
        auto *seq = std::get_if<ast::SeqNode>(&r.builder->getExpr(r.exprId));
        CHECK(seq != nullptr, "Caso D1: AST contem SeqNode principal");
        if (seq) {
            CHECK_EQ(seq->ops[0].word_name, "infix", "Caso D1: op principal e 'infix'");
        }
    }

    // Caso D2: Resolucao de ambiguidade alternativa com parenteses
    {
        ExprParserTest t;
        auto r = t.parse("a prefix (b infix c)");
        CHECK(r.ok, "Caso D2: parse com parenteses resolvendo a ambiguidade a direita");
        auto *seq = std::get_if<ast::SeqNode>(&r.builder->getExpr(r.exprId));
        CHECK(seq != nullptr, "Caso D2: AST contem SeqNode principal");
        if (seq) {
            CHECK_EQ(seq->ops[0].word_name, "prefix", "Caso D2: op principal e 'prefix'");
        }
    }

    // Caso D3: Operadores customizados e indexacoes de array combinados
    {
        ExprParserTest t;
        auto r = t.parse("a nop (b + c) nop d[index]");
        CHECK(r.ok, "Caso D3: parse de combinacao complexa de nop e indexacao");
        auto *seq = std::get_if<ast::SeqNode>(&r.builder->getExpr(r.exprId));
        CHECK(seq != nullptr, "Caso D3: AST contem SeqNode");
        if (seq) {
            CHECK_EQ(seq->operands.size(), 3, "Caso D3: possui exatamente 3 operandos");
            auto *op3 = std::get_if<ast::IndexNode>(&r.builder->getExpr(seq->operands[2]));
            CHECK(op3 != nullptr, "Caso D3: operando 3 e um IndexNode");
        }
    }
}

// ── Runner ────────────────────────────────────────────────────────────

static void test_parser_expr() {
    test_happy_paths();
    test_boundary_cases();
    test_error_handling();
    test_complex_combinatorial();
}

TEST_MAIN(parser_expr)
