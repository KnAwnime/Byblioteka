#pragma once

/// Defines the Float8_e5m2fnuz type (8-bit floating-point) including
/// conversions to standard C types and basic arithmetic operations. Note that
/// arithmetic operations are implemented by converting to floating point and
/// performing the operation in float32.
///
/// Binary configuration remains the same as e5m2:
/// s eeeee mm
/// 1 sign bit
/// 5 exponent bits
/// 2 mantissa bits
///
/// The key differences that e5m2fnuz brings are:
/// bias = 16
/// no infinities or negative zero
/// NaN only when sign bit is 1, rest all 0s
///
/// Implementation based on the paper https://arxiv.org/pdf/2206.02915.pdf and
/// the existing Float8_e4m3fn implementation.

#include <c10/macros/Macros.h>
#include <c10/util/C++17.h>
#include <c10/util/TypeSafeSignMath.h>
#include <c10/util/floating_point_utils.h>

#if defined(__cplusplus) && (__cplusplus >= 201103L)
#include <cstdint>
#elif !defined(__OPENCL_VERSION__)
#include <math.h>
#include <stdint.h>
#endif

#include <iosfwd>
#include <ostream>

namespace c10 {

namespace detail {

/*
 * Convert a 8-bit floating-point number in fp8 E5M2FNUZ format, in bit
 * representation, to a 32-bit floating-point number in IEEE single-precision
 * format, in bit representation.
 *
 * @note The implementation doesn't use any floating-point operations.
 */
#if defined(__CUDA_ARCH__) || defined(__HIP__)
C10_HOST_DEVICE C10_API inline float fp8e5m2fnuz_to_fp32_value(uint8_t) {
  CUDA_KERNEL_ASSERT(false && "e5m2fnuz is not supported by CUDA or HIP");
  return -1.0;
}
#else
C10_API float fp8e5m2fnuz_to_fp32_value(uint8_t input);
#endif

/*
 * Convert a 32-bit floating-point number in IEEE single-precision format to a
 * 8-bit floating-point number in fp8 E5M2 format, in bit representation.
 */
C10_HOST_DEVICE inline uint8_t fp8e5m2fnuz_from_fp32_value(float f) {
  /*
   * Binary representation of 65536.0f, which is the first value not
   * representable (i.e. the first value which would overflow in to the sign
   * bit, resulting in a NaN) in fp8e4m3fnuz range:
   * 1 00000 00 - fp8e5m2fnuz
   * 0 10001111 00000000000000000000000 - fp32
   */
  constexpr uint32_t fnuz_max = UINT32_C(0x8F) << 23;

  /*
   * A mask for converting fp32 numbers lower than fp8e5m2fnuz normal range
   * into denormalized representation.
   * magic number: ((127 - 16) + (23 - 2) + 1)
   */
  constexpr uint32_t denorm_mask = UINT32_C(0x85) << 23;

  uint32_t f_bits = fp32_to_bits(f);

  uint32_t result = 0u;

  /*
   * Extract the sign of the input number into the high bit of the 32-bit word:
   *
   *      +---+----------------------------------+
   *      | S |0000000 00000000 00000000 00000000|
   *      +---+----------------------------------+
   * Bits  31                 0-31
   */
  const uint32_t sign = f_bits & UINT32_C(0x80000000);

  /*
   * Set sign bit to 0
   */
  f_bits ^= sign;

  if (f_bits >= fnuz_max) {
    // NaN -- sign bit set to 1, rest 0s
    return 0x80;
  }

  if (f_bits < (UINT32_C(0x70) << 23) /* 2^-15 in float32 */) {
    // Input exponent is less than -15, the smallest e5m2fnuz exponent, so the
    // number will become subnormal.
    f_bits = fp32_to_bits(fp32_from_bits(f_bits) + fp32_from_bits(denorm_mask));
    result = static_cast<uint8_t>(f_bits - denorm_mask);
    if (result == 0) {
      // fnuz types don't have negative zero.
      return 0;
    }
  } else {
    // resulting mantissa is odd
    uint8_t mant_odd = (f_bits >> 21) & 1;

    // update exponent, rounding bias part 1
    f_bits += ((uint32_t)(16 - 127) << 23) + 0xFFFFF;

    // rounding bias part 2
    f_bits += mant_odd;

    // take the bits!
    result = static_cast<uint8_t>(f_bits >> 21);
  }

  result |= sign >> 24;
  return result;
}

} // namespace detail

struct alignas(1) Float8_e5m2fnuz {
  uint8_t x;

  struct from_bits_t {};
  static constexpr C10_HOST_DEVICE from_bits_t from_bits() {
    return from_bits_t();
  }

  Float8_e5m2fnuz() = default;

  constexpr C10_HOST_DEVICE Float8_e5m2fnuz(uint8_t bits, from_bits_t)
      : x(bits){};
  inline C10_HOST_DEVICE Float8_e5m2fnuz(float value);
  inline C10_HOST_DEVICE operator float() const;
  inline C10_HOST_DEVICE bool isnan() const;
};

C10_API std::ostream& operator<<(
    std::ostream& out,
    const Float8_e5m2fnuz& value);

} // namespace c10

#include <c10/util/Float8_e5m2fnuz-inl.h> // IWYU pragma: keep
