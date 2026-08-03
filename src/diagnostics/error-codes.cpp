#include "error-codes.hpp"

namespace zith::diagnostics {

memory::Optional<ErrorInfo> lookupError(ErrCode code) noexcept {
    switch (code) {
    // Lexical
    case err::UnknownToken:
        return ErrorInfo{code, 'E', "lexical", "Unknown token",
                         "Check for stray or invalid characters in the source"};
    case err::UnclosedString:
        return ErrorInfo{code, 'E', "lexical", "Unclosed string literal",
                         "Add a closing double quote to terminate the string"};
    case err::InvalidEscape:
        return ErrorInfo{code, 'E', "lexical", "Invalid escape sequence",
                         "Use valid escapes: \\n, \\t, \\\", \\\\, \\0"};
    case err::InvalidIntLiteral:
        return ErrorInfo{code, 'E', "lexical", "Invalid integer literal",
                         "Ensure digits are valid for the given base (0x, 0o, 0b)"};
    case err::UnclosedComment:
        return ErrorInfo{code, 'E', "lexical", "Unclosed comment",
                         "Add a closing '*/' to terminate the comment block"};

    // Parse
    case err::ExpectedExpr:
        return ErrorInfo{code, 'E', "parse", "Expected expression",
                         "Write a value, variable, or function call here"};
    case err::ExpectedSemicolon:
        return ErrorInfo{code, 'E', "parse", "Expected semicolon", "Terminate statements with ;"};
    case err::UnclosedParen:
        return ErrorInfo{code, 'E', "parse", "Unclosed parenthesis",
                         "Add a closing ) to match the opening ("};
    case err::ExpectedIdent:
        return ErrorInfo{code, 'E', "parse", "Expected identifier",
                         "A name (variable, function, type) is required here"};
    case err::InvalidImportDepth:
        return ErrorInfo{code, 'E', "parse", "Invalid import depth",
                         "Import depth must be an integer or '..' for infinite depth"};
    case err::ImportError:
        return ErrorInfo{code, 'E', "parse", "Import error",
                         "Check the module path and ensure the file exists"};
    case err::TopLevelLetNotAllowed:
        return ErrorInfo{
            code,
            'E',
            "parse",
            "Top-level declaration",
            "Avoid overusing globals \u2014 prefer passing values via function parameters",
            "`let` bindings are for function/block scope only"};

    case err::DeprecatedSyntax:
        return ErrorInfo{code, 'W', "parse", "Deprecated syntax",
                         "Use the replacement form shown in the message"};

    // Semantic
    case err::UndefinedIdent:
        return ErrorInfo{code, 'E', "semantic", "Undefined identifier",
                         "Bind the name with let or pass it as a parameter"};
    case err::DuplicateDecl:
        return ErrorInfo{code, 'E', "semantic", "Duplicate declaration",
                         "Rename one of the declarations or remove the duplicate"};
    case err::WrongArity:
        return ErrorInfo{
            code, 'E', "semantic", "Wrong number of arguments",
            "Check the function signature and provide the correct number of arguments"};
    case err::UnusedDecl:
        return ErrorInfo{code, 'E', "semantic", "Unused declaration",
                         "Remove the unused binding or prefix with _"};
    case err::NotNamespace:
        return ErrorInfo{
            code, 'E', "semantic", "Not a namespace",
            "Use dot notation only on modules, structs, enums, traits, interfaces, or unions"};
    case err::NoMember:
        return ErrorInfo{code, 'E', "semantic", "No such member",
                         "Check the spelling or visibility of the member in the parent"};
    case err::NoMatchingFn:
        return ErrorInfo{
            code, 'E', "semantic", "No matching function",
            "No function with the provided arguments exists. Check argument types and arity"};
    case err::AmbiguousCall:
        return ErrorInfo{
            code, 'E', "semantic", "Ambiguous call",
            "Multiple functions match the provided arguments. Add explicit type annotations"};
    case err::NotImplemented:
        return ErrorInfo{
            code, 'W', "semantic", "Feature not implemented",
            "This language feature is parsed, but the compiler does not implement it yet"};
    case err::UnsupportedSyntax:
        return ErrorInfo{code, 'E', "semantic", "Unsupported syntax",
                         "This syntax is parsed, but does not have defined semantic behavior yet"};

    // Types
    case err::TypeMismatch:
        return ErrorInfo{code, 'E', "types", "Type mismatch",
                         "Convert the value to the expected type or change the expression"};
    case err::CannotInfer:
        return ErrorInfo{code, 'E', "types", "Cannot infer type",
                         "Add an explicit type annotation to disambiguate"};
    case err::InvalidCast:
        return ErrorInfo{code, 'E', "types", "Invalid cast",
                         "The source and target types are not compatible for casting"};
    case err::CyclicType:
        return ErrorInfo{code, 'E', "types", "Cyclic type",
                         "A type cannot contain itself \u2014 check the type definition"};
    case err::NullDerefUnproven:
        return ErrorInfo{code, 'E', "types", "Null dereference without proof",
                         "Use 'if (x is null) { } else { ... }' or 'for (not (x is null))' "
                         "to prove the value is non-null before dereferencing"};

    // Ownership
    case err::UseAfterMove:
        return ErrorInfo{code, 'E', "ownership", "Use after move",
                         "Re-bind the value before using it, or pass a reference instead"};
    case err::BorrowConflict:
        return ErrorInfo{code, 'E', "ownership", "Borrow conflict",
                         "A mutable reference conflicts with existing borrows"};
    case err::DoubleBorrow:
        return ErrorInfo{code, 'E', "ownership", "Double borrow",
                         "Only one mutable borrow is allowed at a time"};

    // MIR
    case err::InvalidIR:
        return ErrorInfo{code, 'E', "mir", "Invalid IR",
                         "This is an internal compiler error \u2014 report it"};
    case err::Unreachable:
        return ErrorInfo{code, 'E', "mir", "Unreachable code",
                         "Remove the dead code or add a condition guard"};

    // Runtime
    case err::IndexOutOfBounds:
        return ErrorInfo{code, 'R', "runtime", "Index out of bounds",
                         "Ensure the index is less than the array or slice length"};
    case err::DivisionByZero:
        return ErrorInfo{code, 'R', "runtime", "Division by zero",
                         "Check the divisor is non-zero before dividing"};
    case err::NullDeref:
        return ErrorInfo{code, 'R', "runtime", "Null dereference",
                         "Check if the optional value is Some before unwrapping"};
    case err::Panic:
        return ErrorInfo{code, 'R', "runtime", "Panic",
                         "An unrecoverable error occurred \u2014 check the error context"};

    default:
        return nullptr;
    }
}

} // namespace zith::diagnostics
