#include "frontend/parser/actions.hpp"

// Handwritten parser hooks declared in parser.rules are implemented here.

using Parser = generated_parser::Parser<sample::ParseOutput>;
using Token = generated_lexer::Token;

namespace hooks::parser {

Recovery recover(Parser &parser, const Token &token);

void beginModule(Parser &parser, const Token &token);
void sawTopIdentifier(Parser &parser, const Token &token);
void sawSemi(Parser &parser, const Token &token);
void enterType(Parser &parser, const Token &token);
void moduleName(Parser &parser, const Token &token);
void globalName(Parser &parser, const Token &token);
void leafExpr(Parser &parser, const Token &token);
void binaryExpr(Parser &parser, const Token &token);
void unaryExpr(Parser &parser, const Token &token);
void typeName(Parser &parser, const Token &token);

} // namespace hooks::parser
