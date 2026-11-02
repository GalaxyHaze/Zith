#include <stdint.h>

int main(void) {
    uint64_t state = UINT64_C(88172645463325252);
    const uint64_t expected = UINT64_C(@@EXPECTED@@);

    for (uint64_t i = 0; i < UINT64_C(@@ITERATIONS@@); ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
    }

    return state == expected ? 0 : 1;
}
