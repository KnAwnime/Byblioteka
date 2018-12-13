#include <ATen/ATen.h>
#include <ATen/InitialTensorOptions.h>
#include <ATen/NativeFunctions.h>
#include <ATen/cuda/CUDAApplyUtils.cuh>
#include <ATen/cuda/CUDAContext.h>
#include <ATen/native/TensorFactories.h>
#include <c10/util/Exception.h>

#include <THC/THCGeneral.h>
#include <THC/THCThrustAllocator.cuh>
#include <thrust/device_ptr.h>
#include <thrust/sort.h>
#include <thrust/execution_policy.h>
#include <thrust/sequence.h>

#include <algorithm>
#include <cstddef>
#include <cmath>

namespace at {
namespace native {

Tensor& eye_out_cuda(Tensor& result, int64_t n) {
  return at::native::eye_out_cuda(result, n, /*m=*/-1);
}

Tensor& eye_out_cuda(Tensor& result, int64_t n, int64_t m) {
  AT_CHECK(n >= 0, "n must be greater or equal to 0, got ", n);

  if(m < 0) {
    m = n;
  }

  result.resize_({n, m});
  result.zero_();

  int64_t sz = std::min<int64_t>(n, m);
  int64_t stride = result.stride(0) + result.stride(1);

  Tensor diag = result.as_strided({sz}, {stride});
  diag.fill_(1);
  return result;
}

Tensor empty_cuda(IntList size, const TensorOptions& options) {
  AT_ASSERT(options.backend() == at::Backend::CUDA);
  AT_ASSERT(!options.is_variable());  // is_variable should have been 'unpacked'

  auto* allocator = at::cuda::getCUDADeviceAllocator();
  int64_t nelements = prod_intlist(size);
  auto dtype = options.dtype();
  auto storage_impl = c10::make_intrusive<StorageImpl>(
    dtype,
    nelements,
    allocator->allocate(nelements * dtype.itemsize()),
    allocator,
    /*resizeable=*/true);

  auto tensor = detail::make_tensor<TensorImpl>(storage_impl, CUDATensorId(), false);
  // Default TensorImpl has size [0]
  if (size.size() != 1 || size[0] != 0) {
    tensor.unsafeGetTensorImpl()->set_sizes_contiguous(size);
  }
  return tensor;
}

Tensor& randperm_out_cuda(Tensor& result, int64_t n, Generator* generator) {
  AT_CHECK(n >= 0, "n must be non-negative, got", n);
  AT_CHECK(at::scalar_tensor(n, result.options()).defined(),
  "n is too large for result tensor type: '", result.type().toString(), "'");

  result.resize_({n});

  if (result.type().scalarType() == at::ScalarType::Half) {
    auto result_float = at::empty({n}, initialTensorOptions().device(Device(DeviceType::CUDA)));
    result.copy_(randperm_out_cuda(result_float, n, generator));
  } else {
    if (n < 30000) {  // For small inputs, we offload it to CPU instead.
      auto result_cpu = at::empty({n}, result.options().device(kCPU));
      randperm_out(result_cpu, n, generator);
      result.copy_(result_cpu);
    } else {
      // Generate random values for the keys array
      AT_DISPATCH_ALL_TYPES(
        result.type(), "randperm_out_cuda", [&] {
          auto keys = at::empty(result.sizes(), result.options()).random_(generator);

          auto result_data = thrust::device_ptr<scalar_t>(result.data<scalar_t>());
          auto keys_data = thrust::device_ptr<scalar_t>(keys.data<scalar_t>());

          auto state = globalContext().getTHCState();
          THCThrustAllocator thrustAlloc(state);
          auto policy = thrust::cuda::par(thrustAlloc).on(at::cuda::getCurrentCUDAStream());

          thrust::sequence(policy, result_data, result_data + n);

          // Use the sorted order of keys to rearrange the result array
          thrust::sort_by_key(policy, keys_data, keys_data + n, result_data);
        }
      );
    }
  }

  return result;
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ triangle ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

namespace {
// f: the number of elements in the first row of the trapezoid.
// x: the index of the target coordinates ordered by row and then column.
// Assume x corresponding to coordinates (row, col) in the trapezoid, where
// row and col start from 0, then we have:
//     (f + f + row - 1) * row / 2 <= x < (f + f + row) * (row + 1) / 2.    [*]
// Therefore, row is the maximum integer satisfying the following inequality:
//              (row + 2f - 1)row <= 2x
//         row^2 + (2f-1)row - 2x <= 0.
// Based on the formula of root, we have:
// a = 1
// b = 2f - 1
// c = -2x
// Use the root on the right, intuitively because it is where the left-hand side
// formular flips from negative to positive. Full proof can be derived from the
// second inequality in [*].
// row = floor((-b + sqrt(b^2 - 4c)) / 2)
// col = x - (f + f + row - 1) * row / 2
__device__
inline void get_coordinate_in_tril_trapezoid(
    int64_t f, int64_t x, int64_t & row, int64_t & col) {
  f <<= 1;
  auto b = f - 1;
  auto c = - (x << 1);
  row = (int64_t)floorf((-b + sqrtf((float)(b * b - 4 * c))) / 2);
  col = x - ((f + row - 1) * row >> 1);
}

// f: the number of elements in the first row of the bottom trapezoid.
// x: the index of the target coordinates ordered by row and then column.
// Note that in triu, the trapezoid is upside down.
// Assume x corresponding to coordinates (row, col) in the trapezoid, where
// row and col start from 0, then we have:
//     (f + f - row + 1) * row / 2 <= x < (f + f - row) * (row + 1) / 2.    [*]
// Therefore, row is the maximum integer satisfying the following inequality:
//              (-row + 2f + 1)row <= 2x
//          row^2 - (2f+1)row + 2x >= 0.
// Based on the formula of root, we have:
// a = 1
// b = -1 - 2f
// c = 2x
// Use the root on the left, intuitively because it is where the left-hand side
// formular flips from positive to negative. Full proof can be derived from the
// second inequality in [*].
// row = floor((-b - sqrt(b^2 - 4c)) / 2)
// col = x - (f + f - row + 1) * row / 2
__device__
inline void get_coordinate_in_triu_trapezoid(
    int64_t f, int64_t x, int64_t & row, int64_t & col) {
  f <<= 1;
  auto b = -1 - f;
  auto c = x << 1;
  row = (int64_t)floorf((-b - sqrtf((float)(b * b - 4 * c))) / 2);
  col = x - ((f - row + 1) * row >> 1) + row;
}

} // namespace

template <typename scalar_t>
__global__
void tril_indices_kernel(scalar_t * tensor,
                         int64_t row_offset,
                         int64_t m_first_row,
                         int64_t col,
                         int64_t trapezoid_size,
                         int64_t tril_size) {
  int64_t linear_index = blockIdx.x * blockDim.x + threadIdx.x;

  if (linear_index < tril_size) {
    int64_t r, c;
    if (linear_index < trapezoid_size) {
      get_coordinate_in_tril_trapezoid(m_first_row, linear_index, r, c);
    } else {
      auto surplus = linear_index - trapezoid_size;
      // add trapezoid height: m_last_row (col) - m_first_row + 1
      r = surplus / col + col - m_first_row + 1;
      c = surplus % col;
    }
    r += row_offset;

    tensor[linear_index] = r;
    tensor[linear_index + tril_size] = c;
  }
}

Tensor tril_indices_cuda(
    int64_t row, int64_t col, int64_t offset, const TensorOptions& options) {
  check_args(row, col, options);

  auto tril_size = get_tril_size(row, col, offset);
  auto tensor = empty_cuda({2, tril_size}, options);

  if (tril_size > 0) {
    auto m_first_row = offset > 0 ?
      std::min<int64_t>(col, 1 + offset) : // upper bounded by col
      row + offset > 0; // either 0 or 1
    auto trapezoid_row_offset = std::max<int64_t>(0, -offset);
    auto rectangle_row_offset = trapezoid_row_offset + col - m_first_row + 1;
    int64_t rectangle_size = 0;
    if (rectangle_row_offset < row) {
      rectangle_size = (row - rectangle_row_offset) * col;
    }

    dim3 dim_block = cuda::getApplyBlock();
    dim3 dim_grid;
    // using tril_size instead of tensor.numel(), as each thread takes care of
    // two elements in the tensor.
    AT_CHECK(
      cuda::getApplyGrid(tril_size, dim_grid, tensor.get_device()),
      "unable to get dim grid");

    AT_DISPATCH_ALL_TYPES_AND_HALF(tensor.type(), "tril_indices_cuda", [&] {
      tril_indices_kernel<<<
          dim_grid, dim_block, 0, at::cuda::getCurrentCUDAStream()>>>(
        tensor.data<scalar_t>(),
        trapezoid_row_offset,
        m_first_row,
        col,
        tril_size - rectangle_size,
        tril_size);
    });
  }

  return tensor;
}

template <typename scalar_t>
__global__
void triu_indices_kernel(scalar_t * tensor,
                         int64_t col_offset,
                         int64_t m_first_row,
                         int64_t col,
                         int64_t rectangle_size,
                         int64_t triu_size) {
  int64_t linear_index = blockIdx.x * blockDim.x + threadIdx.x;

  if (linear_index < triu_size) {
    int64_t r, c;
    if (linear_index < rectangle_size) {
      r = linear_index / col;
      c = linear_index % col;
    } else {
      get_coordinate_in_triu_trapezoid(
        m_first_row, linear_index - rectangle_size, r, c);
      r += rectangle_size / col;
    }

    c += col_offset;
    tensor[linear_index] = r;
    tensor[linear_index + triu_size] = c;
  }
}

Tensor triu_indices_cuda(
    int64_t row, int64_t col, int64_t offset, const TensorOptions& options) {
  check_args(row, col, options);

  auto triu_size = row * col - get_tril_size(row, col, offset - 1);
  auto tensor = empty_cuda({2, triu_size}, options);

  if (triu_size > 0) {
    auto m_first_row = offset > 0 ?
      std::max<int64_t>(col - offset, 0) : // upper bounded by col
      col;

    int64_t rectangle_size = 0;
    if (offset < 0) {
      rectangle_size = std::min<int64_t>(row, -offset) * col;
    }

    dim3 dim_block = cuda::getApplyBlock();
    dim3 dim_grid;
    // using triu_size instead of tensor.numel(), as each thread takes care of
    // two elements in the tensor.
    AT_CHECK(
      cuda::getApplyGrid(triu_size, dim_grid, tensor.get_device()),
      "unable to get dim grid");

    AT_DISPATCH_ALL_TYPES_AND_HALF(tensor.type(), "triu_indices_cuda", [&] {
      triu_indices_kernel<<<
          dim_grid, dim_block, 0, at::cuda::getCurrentCUDAStream()>>>(
        tensor.data<scalar_t>(),
        std::max<int64_t>(0, offset),
        m_first_row,
        col,
        rectangle_size,
        triu_size);
    });
  }

  return tensor;
}

}} // namespace at::native
