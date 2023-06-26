#define TORCH_ASSERT_NO_OPERATORS
#include <ATen/Dispatch.h>
#include <ATen/Parallel.h>
#include <ATen/cpu/vec/vec.h>
#include <ATen/cpu/vec/functional.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/native/cpu/Loops.h>

#include <ATen/native/Fill.h>
#include <c10/core/Scalar.h>

namespace at::native {
namespace {


template <typename scalar_t>
void fill_non_native_type(TensorIterator& iter, const Scalar& value_scalar) {
  auto value = value_scalar.to<scalar_t>().x;
  using H = typename std::make_signed<decltype(value)>::type;  // Signed type has more acceleration
  // Reserve the representation of value. static_cast<H>(value) is implementation defined.
  H val = *reinterpret_cast<H*>(std::addressof(value));
  cpu_kernel_vec</*check_dynamic_cast=*/false>(
      iter,
      [val]() -> H { return val; },
      [val]() { return Vectorized<H>(val); });
}

template <>
void fill_non_native_type<c10::complex<at::Half>>(TensorIterator& iter, const Scalar& value_scalar) {
  static_assert(sizeof(c10::complex<at::Half>) == sizeof(int32_t), "Size of ComplexHalf should be 32-bits");
  auto value = c10::complex<at::Half>(value_scalar.to<c10::complex<float>>());
  auto val = *reinterpret_cast<int32_t*>(std::addressof(value));
  cpu_kernel_vec</*check_dynamic_cast=*/false>(
      iter,
      [val]() -> int32_t { return val; },
      [val]() { return Vectorized<int32_t>(val); });
}

void fill_kernel(TensorIterator& iter, const Scalar& value_scalar) {
  if (iter.dtype() == ScalarType::Half) {
    fill_non_native_type<at::Half>(iter, value_scalar);
  } else if (iter.dtype() == ScalarType::BFloat16) {
    fill_non_native_type<at::BFloat16>(iter, value_scalar);
  } else if (iter.dtype() == ScalarType::ComplexHalf) {
    fill_non_native_type<c10::complex<at::Half>>(iter, value_scalar);
  } else {
    AT_DISPATCH_ALL_TYPES_AND_COMPLEX_AND(at::ScalarType::Bool, iter.dtype(), "fill_cpu", [&]() {
      scalar_t value = value_scalar.to<scalar_t>();
      cpu_kernel_vec(
          iter,
          [=]() -> scalar_t { return value; },
          [=]() { return Vectorized<scalar_t>(value); });
    });
  }
}

} // namespace

// This kernel is slower with AVX512 than with AVX2.
#ifndef CPU_CAPABILITY_AVX512
REGISTER_DISPATCH(fill_stub, &fill_kernel);
#else
REGISTER_NO_AVX512_DISPATCH(fill_stub);
#endif

} // namespace at::native
