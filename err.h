#ifndef ERR_H
#define ERR_H

#include <cstdint>

template<typename T> struct Option {
    uint8_t err = true;
    T data;
};

template<typename Err, typename T> struct Result {
    Err err;
    T data;
};

#endif // ERR_H
