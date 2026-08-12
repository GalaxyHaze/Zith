#include "zith/cache/byte-cache.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    const std::string path = "byte-cache-test.bin";
    std::filesystem::remove(path);

    {
        zith::cache::ByteCache cache(path);
        cache.put("hello", "world");
        cache.put("empty", "");
        expect(cache.contains("hello"), "contains after put");
        expect(cache.get("hello").value() == "world", "get after put");
        expect(cache.size() == 2, "size after puts");
        expect(cache.remove("hello"), "remove existing key");
        expect(!cache.contains("hello"), "contains after remove");
        expect(cache.save(), "save");
    }

    {
        zith::cache::ByteCache cache(path);
        expect(cache.load(), "load");
        expect(cache.get("empty").has_value(), "load empty value");
        expect(cache.get("empty").value().empty(), "empty value stays empty");
    }

    std::filesystem::remove(path);
    if (failures == 0)
        std::cout << "byte-cache OK\n";
    return failures == 0 ? 0 : 1;
}
