#include <stdint.h>

typedef struct {
    int32_t *ptr;
    int64_t len;
} Slice;

static int32_t values[3] = {10, 20, 30};

Slice make_slice(void) {
    Slice result = {values, 3};
    return result;
}

int apply(int (*fn)(int), int value) {
    return fn(value) + 10;
}

int double(int value) {
    return value * 2;
}
