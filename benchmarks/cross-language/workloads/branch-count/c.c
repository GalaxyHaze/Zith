#include <stdint.h>

int main(void) {
    uint64_t count = 0;
    const uint64_t expected = UINT64_C(@@EXPECTED@@);

    for (uint64_t i = 0; i < UINT64_C(@@ITERATIONS@@); ++i) {
        if (i % UINT64_C(7) == 0 || i % UINT64_C(11) == 0) {
            ++count;
        }
    }

    return count == expected ? 0 : 1;
}
