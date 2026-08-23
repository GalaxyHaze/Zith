#pragma once

#include <cstdio>

#if defined(ZITH_ENABLE_DEBUG_PRINT) && ZITH_ENABLE_DEBUG_PRINT
#define ZITH_DEBUG_PRINT(...) std::fprintf(stderr, "[zith-debug] " __VA_ARGS__)
#else
#define ZITH_DEBUG_PRINT(...) ((void)0)
#endif
