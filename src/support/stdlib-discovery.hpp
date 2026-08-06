#pragma once

#include <string>
#include <vector>

namespace zith::support {

/// Discover stdlib root paths relative to the compiler binary.
///
/// Checks, in order:
/// 1. The ZITH_STDLIB environment variable, if set and pointing to an existing directory.
/// 2. <binary_dir>/../share/zith/stdlib (installed layout).
/// 3. <binary_dir>/../stdlib (dev/build layout).
///
/// Returns a vector of zero, one, or two existing directory paths.
std::vector<std::string> findStdlibRoots();

} // namespace zith::support
