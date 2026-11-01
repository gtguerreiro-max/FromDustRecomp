#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

// Big-endian value stored in guest memory.
template<typename T>
struct be
{
    T raw;

    static T Swap(T value)
    {
        if constexpr (sizeof(T) == 1)
            return value;
        else if constexpr (sizeof(T) == 2)
        {
            uint16_t v;
            memcpy(&v, &value, 2);
            v = __builtin_bswap16(v);
            memcpy(&value, &v, 2);
            return value;
        }
        else if constexpr (sizeof(T) == 4)
        {
            uint32_t v;
            memcpy(&v, &value, 4);
            v = __builtin_bswap32(v);
            memcpy(&value, &v, 4);
            return value;
        }
        else
        {
            static_assert(sizeof(T) == 8);
            uint64_t v;
            memcpy(&v, &value, 8);
            v = __builtin_bswap64(v);
            memcpy(&value, &v, 8);
            return value;
        }
    }

    be() = default;
    be(T value) : raw(Swap(value)) {}

    T get() const { return Swap(raw); }
    operator T() const { return get(); }
    be& operator=(T value) { raw = Swap(value); return *this; }
};

static_assert(sizeof(be<uint32_t>) == 4);
