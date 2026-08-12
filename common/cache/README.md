# zith-cache

Small persistent byte cache used as a `common/cache` submodule in Zith-Lang.

The library stays deliberately simple: a key/value byte store with an
in-memory view and optional persistence to a binary file. It has no third-party
dependencies and is not wired into the compiler pipeline yet.

## Build

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## API

```cpp
#include "zith/cache/byte-cache.hpp"

zith::cache::ByteCache cache("cache.bin");
cache.put("module", "artifact-bytes");
auto value = cache.get("module");
```

`ByteCache::save()` writes a small binary format with per-entry length-prefixed
keys and values. `ByteCache::load()` clears current contents before reading.
