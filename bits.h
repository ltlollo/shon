#ifndef BITS_H
#define BITS_H

#include <cstdint>
#include <byteswap.h>

// returns the numner of equal preceding bits starting from MSB
// eg 0b11101000..., 0b1101001001... -> 2
constexpr unsigned prefix(const uint64_t f, const uint64_t s) {
    uint64_t v = f^s;
    if (v == 0) {
        return 64;
    }
    unsigned i = 0, range = 32;
    do {
        if (v>>range) {
            v >>= range;
        } else {
            i += range;
        }
    } while(range /= 2);
    return i;
}

// returns the numner of equal preceding bits starting from LSB
// eg 0b...10101001, 0b...10010001 -> 3
constexpr unsigned suffix(const uint64_t f, const uint64_t s) {
    uint64_t v = f^s;
    if (v == 0) {
        return 64;
    }
    unsigned i = 0, range = 32;
    do {
        if (v<<range) {
            v <<= range;
        } else {
            i += range;
        }
    } while (range /= 2);
    return i;
}

#endif // BITS_H
