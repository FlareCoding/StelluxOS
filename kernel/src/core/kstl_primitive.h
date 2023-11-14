#ifndef KSTL_PRIMITIVE_H
#define KSTL_PRIMITIVE_H
#include <ktypes.h>

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
} // namespace kstl

#endif
