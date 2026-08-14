#pragma once

#include "codegen/codegen.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace toolkit::codegen::vm {

struct StdReturn {
    uint8_t raw[8] = {};
};

class VM {
public:
    [[nodiscard]] StdReturn run(FunctionCode &function, std::span<const uint8_t> args);
};

[[nodiscard]] bool paramsMatchLayout(const FunctionCode &function,
                                     std::span<const uint8_t> args) noexcept;

} // namespace toolkit::codegen::vm
