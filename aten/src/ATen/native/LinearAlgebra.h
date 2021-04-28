#pragma once

#include <ATen/ATen.h>
#include <ATen/native/DispatchStub.h>
#include <c10/util/Optional.h>
#include <ATen/native/TensorIterator.h>

namespace at { namespace native {

using addr_fn = void (*)(TensorIterator &, const Scalar& beta, const Scalar& alpha);
DECLARE_DISPATCH(addr_fn, addr_stub);

using linalg_vector_norm_fn = void(*)(TensorIterator &, Scalar);
DECLARE_DISPATCH(linalg_vector_norm_fn, linalg_vector_norm_stub);

C10_EXPORT Tensor& addmm_cpu_out(const Tensor& self, const Tensor& mat1, const Tensor& mat2, const Scalar& beta, const Scalar& alpha, Tensor &result);

}} // namespace at::native
