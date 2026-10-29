// Ghidra's "undefined4"/"undefined8": raw 32/64-bit values whose type the decompiler could not infer.
// Constructing them from a float/double keeps the bit pattern (the original passed floats in them);
// constructing from an integer keeps the value (an integer constant is just a bit pattern too).
#pragma once
#include <stdint.h>
#include <string.h>
#include <type_traits>

struct UD4 {
    uint32_t v;
    UD4() : v(0) {}
    template <class T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
    UD4(T x) : v((uint32_t)x) {}
    UD4(float f) { memcpy(&v, &f, 4); }
    UD4(double d) { float f = (float)d; memcpy(&v, &f, 4); }
    UD4(long double d) { float f = (float)d; memcpy(&v, &f, 4); }
    operator uint32_t() const { return v; }
};

struct UD8 {
    uint64_t v;
    UD8() : v(0) {}
    template <class T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
    UD8(T x) : v((uint64_t)x) {}
    UD8(float f) { double d = f; memcpy(&v, &d, 8); }
    UD8(double d) { memcpy(&v, &d, 8); }
    UD8(long double d) { double dd = (double)d; memcpy(&v, &dd, 8); }
    operator double() const { double d; memcpy(&d, &v, 8); return d; }
};
// bit operations on raw 64-bit values
inline uint64_t operator>>(UD8 a, int n) { return a.v >> n; }
inline uint64_t operator<<(UD8 a, int n) { return a.v << n; }
inline uint64_t operator&(UD8 a, uint64_t m) { return a.v & m; }
inline uint64_t operator|(UD8 a, uint64_t m) { return a.v | m; }
inline bool operator==(UD8 a, int b) { return a.v == (uint64_t)b; }
inline bool operator!=(UD8 a, int b) { return a.v != (uint64_t)b; }
inline bool operator==(UD8 a, UD8 b) { return a.v == b.v; }
inline bool operator!=(UD8 a, UD8 b) { return a.v != b.v; }
