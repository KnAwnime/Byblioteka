#pragma once

#include <c10/core/ScalarType.h>
#include <c10/util/Half.h>
#include <c10/util/BFloat16.h>


namespace c10 {

// Note [Implicit conversion between signed and unsigned]
// C and C++ have a lovely set of implicit conversion rules, where casting
// signed integral values to unsigned integral values is always valid
// (it basically treats the value as if using modulo arithmetic), however
// converting negative floating point values to unsigned integral types
// is UB! This means that: (double)-1 -> (int64_t)-1 -> (uint8_t)255 is
// guaranteed to look like this, but we have (double)-1 -> (uint8_t)<ANYTHING>
// because it's UB. This also makes UBSan really angry.
//
// I think those rules are stupid and we really shouldn't conform to them.
// The structs below ensure that for all unsigned types we use (currently
// only uint8_t), we will do an intermediate convertion via int64_t,
// to ensure that any negative values are wrapped around correctly.
//
// Note that conversions from doubles to signed integral types that can't
// represent a particular value after truncating the fracitonal part are UB as well,
// but fixing them is not as simple as adding an int64_t intermediate, beacuse the
// int64_t -> <smaller signed type> conversion is UB for those large values anyway.
// I guess in that case we just have to live with that, but it's definitely less
// surprising than the thing above.
//
// For the curious:
//   https://en.cppreference.com/w/cpp/language/implicit_conversion
//   The relevant paragraph is "Floating-integral conversions".

template <typename T>
struct inter_copy_type {
  using type = T;
};

template <>
struct inter_copy_type<uint8_t> {
  using type = int64_t;
};

template <typename T>
using inter_copy_type_t = typename inter_copy_type<T>::type;

template <typename dest_t, typename src_t>
C10_HOST_DEVICE inline dest_t static_cast_with_inter_type(src_t src) {
  return static_cast<dest_t>(
    static_cast<inter_copy_type_t<dest_t>>(src));
}

#ifdef C10_HOST_DEVICE
#define ERROR_UNSUPPORTED_CAST assert(false);
#else
#define ERROR_UNSUPPORTED_CAST TORCH_CHECK(false, "Unexpected scalar type");
#endif

// Fetch a value with dynamic type src_type from ptr, and cast it to static type dest_t.
#define FETCH_AND_CAST_CASE(type, scalartype) case ScalarType::scalartype: return static_cast_with_inter_type<dest_t>(*(const type *)ptr);
#define FETCH_AND_CAST_COMPLEX_CASE(type, scalartype) case ScalarType::scalartype: return static_cast_with_inter_type<dest_t>(std::real(*(const type *)ptr));
template<typename dest_t>
C10_HOST_DEVICE inline dest_t fetch_and_cast(const ScalarType src_type, const void *ptr) {
  switch (src_type) {
    AT_FORALL_SCALAR_TYPES_AND3(Bool, Half, BFloat16, FETCH_AND_CAST_CASE)
#ifndef C10_HOST_DEVICE
    AT_FORALL_COMPLEX_TYPES(FETCH_AND_CAST_COMPLEX_CASE)
#endif
    default:;
  }
  ERROR_UNSUPPORTED_CAST
  return dest_t(0); // just to avoid compiler warning
}

// Cast a value with static type src_t into dynamic dest_type, and store it to ptr.
#define CAST_AND_STORE_CASE(type, scalartype) case ScalarType::scalartype: *(type *)ptr = static_cast_with_inter_type<type>(value); return;
template<typename src_t>
C10_HOST_DEVICE inline void cast_and_store(const ScalarType dest_type, void *ptr, src_t value) {
  switch (dest_type) {
    AT_FORALL_SCALAR_TYPES_AND3(Bool, Half, BFloat16, CAST_AND_STORE_CASE)
    default:;
  }
  ERROR_UNSUPPORTED_CAST
}

#ifndef C10_HOST_DEVICE
template<>
inline void cast_and_store<std::complex<float>>(const ScalarType dest_type, void *ptr, std::complex<float> value_) {
  auto value = std::real(value_);
  switch (dest_type) {
    AT_FORALL_SCALAR_TYPES_AND3(Bool, Half, BFloat16, CAST_AND_STORE_CASE)
    default:;
  }
  ERROR_UNSUPPORTED_CAST
}
template<>
inline void cast_and_store<std::complex<double>>(const ScalarType dest_type, void *ptr, std::complex<double> value_) {
  auto value = std::real(value_);
  switch (dest_type) {
    AT_FORALL_SCALAR_TYPES_AND3(Bool, Half, BFloat16, CAST_AND_STORE_CASE)
    default:;
  }
  ERROR_UNSUPPORTED_CAST
}
#endif

#define DEFINE_UNCASTABLE(T, _)                                                   \
template<>                                                                        \
inline T fetch_and_cast<T>(const ScalarType, const void *) {                      \
  ERROR_UNSUPPORTED_CAST                                                          \
  return T(0); /* just to avoid compiler warning */                               \
}                                                                                 \
template<>                                                                        \
inline void cast_and_store<T>(const ScalarType, void *, T) {                      \
  ERROR_UNSUPPORTED_CAST                                                          \
}

AT_FORALL_QINT_TYPES(DEFINE_UNCASTABLE)

#undef FETCH_AND_CAST_CASE
#undef FETCH_AND_CAST_COMPLEX_CASE
#undef CAST_AND_STORE_CASE
#undef DEFINE_UNCASTABLE
#undef ERROR_UNSUPPORTED_CAST

}  // namespace c10
