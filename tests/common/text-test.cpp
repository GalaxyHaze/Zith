#include "common/text/parse.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main() {
    using common::text::parseBool;
    using common::text::parseInt;
    using common::text::parseLong;
    using common::text::parseString;
    using common::text::parseStringList;

    bool ok = true;

    bool flag = false;
    ok = ok && parseBool("true", flag) && flag;
    ok = ok && parseBool("false", flag) && !flag;
    ok = ok && !parseBool("nope", flag);

    int number = 0;
    ok = ok && parseInt("42", number) && number == 42;
    ok = ok && !parseInt("abc", number);

    long big = 0;
    ok = ok && parseLong("123456789012345", big) && big == 123456789012345L;
    ok = ok && !parseLong("12x", big);

    std::string text;
    ok = ok && parseString("\"hello\\n\"", text) && text == "hello\n";
    ok = ok && !parseString("unterminated", text);

    std::vector<std::string> items;
    ok = ok && parseStringList("[\"a\", \"b\"]", items);
    ok = ok && items.size() == 2 && items[0] == "a" && items[1] == "b";
    items.clear();
    ok = ok && parseStringList("[]", items) && items.empty();
    ok = ok && !parseStringList("[a", items);

    if (!ok) {
        std::fprintf(stderr, "text basics failed\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
