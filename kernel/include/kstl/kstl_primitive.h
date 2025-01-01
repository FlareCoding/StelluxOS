#ifndef KSTL_PRIMITIVE_H
#define KSTL_PRIMITIVE_H
#include <types.h>

namespace kstl {
template <typename T>
struct is_primitive {
    static const bool value = false;
};

template <> struct is_primitive<bool> { static const bool value = true; };

template <> struct is_primitive<char> { static const bool value = true; };
template <> struct is_primitive<signed char> { static const bool value = true; };
template <> struct is_primitive<unsigned char> { static const bool value = true; };
template <> struct is_primitive<wchar_t> { static const bool value = true; };
template <> struct is_primitive<char16_t> { static const bool value = true; };
template <> struct is_primitive<char32_t> { static const bool value = true; };

template <> struct is_primitive<short> { static const bool value = true; };
template <> struct is_primitive<unsigned short> { static const bool value = true; };
template <> struct is_primitive<int> { static const bool value = true; };
template <> struct is_primitive<unsigned int> { static const bool value = true; };
template <> struct is_primitive<long> { static const bool value = true; };
template <> struct is_primitive<unsigned long> { static const bool value = true; };
template <> struct is_primitive<long long> { static const bool value = true; };
template <> struct is_primitive<unsigned long long> { static const bool value = true; };

template <> struct is_primitive<float> { static const bool value = true; };
template <> struct is_primitive<double> { static const bool value = true; };
template <> struct is_primitive<long double> { static const bool value = true; };

template <typename T>
struct is_primitive<T*> { static const bool value = true; };

static const size_t npos = static_cast<size_t>(-1);

// Custom enable_if implementation
template <bool Condition, typename T = void>
struct enable_if {};

// Specialization when Condition is true
template <typename T>
struct enable_if<true, T> {
    typedef T type;
};

// Base template
template <typename T>
struct is_void {
    static const bool value = false;
};

// Specialization for void
template <>
struct is_void<void> {
    static const bool value = true;
};

template <typename T>
__force_inline__ typename enable_if<is_primitive<T>::value, T>::type
min(const T& x, const T& y) {
    return (x < y) ? x : y;
}

template <typename T>
__force_inline__ typename enable_if<is_primitive<T>::value, T>::type
max(const T& x, const T& y) {
    return (x > y) ? x : y;
}
} // namespace kstl

#endif // KSTL_PRIMITIVE_H
