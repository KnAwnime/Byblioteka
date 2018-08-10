// Returns unique elements of input tensor.

#include "ATen/ATen.h"
#include "ATen/Dispatch.h"

#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace at {
namespace native{

namespace {

template <typename scalar_t>
std::tuple<Tensor, Tensor> _unique_cpu_template(
    const Tensor& self,
    const bool sorted,
    const bool return_inverse) {
  const Tensor& input = self.contiguous();
  const scalar_t* input_data = input.data<scalar_t>();
  std::unordered_set<scalar_t> set(input_data, input_data + input.numel());
  Tensor output = at::empty({static_cast<int64_t>(set.size())}, input.type());
  scalar_t* output_data = output.data<scalar_t>();

  if (sorted) {
    std::vector<scalar_t> vec(set.begin(), set.end());
    std::sort(vec.begin(), vec.end());
    std::copy(vec.begin(), vec.end(), output_data);
  } else {
    std::copy(set.begin(), set.end(), output_data);
  }

  Tensor inverse_indices = at::empty({0}, self.type().toScalarType(kLong));
  if (return_inverse) {
    inverse_indices.resize_(input.sizes());
    int64_t* inverse_indices_data = inverse_indices.data<int64_t>();
    std::unordered_map<scalar_t, int64_t> inverse_map;
    inverse_map.reserve(output.numel());
    for (int i = 0; i < output.numel(); ++i) {
      inverse_map[output_data[i]] = i;
    }
    for (int i = 0; i < input.numel(); ++i) {
      inverse_indices_data[i] = inverse_map[input_data[i]];
    }
  }
  return std::make_tuple(output, inverse_indices);
}

template <typename scalar_t>
std::tuple<Tensor, Tensor> _unique_dim_cpu_template(
    const Tensor& self,
    const int64_t dim,
    const bool return_inverse) {
  // reshape tensor as [dim, -1]
  Tensor input_flat = self.transpose(dim, 0);
  std::vector<int64_t> orig_sizes(input_flat.sizes().begin(), input_flat.sizes().end());
  input_flat = input_flat.contiguous().view({input_flat.size(0), -1});

  std::vector<int64_t> indices(input_flat.size(0));
  std::iota(indices.begin(), indices.end(), 0);
  int64_t numel = input_flat.size(1);
  scalar_t* input_flat_ptr = ((scalar_t*)input_flat.data_ptr());

  // sort indices using data
  std::sort(indices.begin(), indices.end(),
    [&](int64_t a, int64_t b) -> bool {
      for (int64_t i = 0; i < numel; ++i) {
        scalar_t lhs = input_flat_ptr[i + a * numel];
        scalar_t rhs = input_flat_ptr[i + b * numel];
        if (lhs < rhs) {
          return true;
        } else if (lhs > rhs) {
          return false;
        }
      }
      return false;
    });

  Tensor input_sorted = at::empty(input_flat.sizes(), input_flat.type());
  for (int i = 0; i < indices.size(); ++i) {
    input_sorted[i] = input_flat[indices[i]];
  }
 
  // pre-calculate mask for inverse_indices
  Tensor mask = at::empty(input_sorted.size(0), self.type().toScalarType(kLong));
  mask[0] = 1;
  int mask_idx = 1;

  std::vector<Tensor> input_unbind = at::unbind(input_sorted, 0);
  auto last = std::unique(input_unbind.begin(), input_unbind.end(), [&](Tensor a, Tensor b) {
    bool eq = at::equal(a, b);
    if (return_inverse) {
      if (!eq) {
        mask[mask_idx++] = 1;
      } else {
        mask[mask_idx++] = 0;
      }
    }
    return eq;
  });
  input_unbind.erase(last, input_unbind.end());

  // reshape back
  auto output = at::stack(input_unbind, 0);
  std::vector<int64_t> new_sizes(orig_sizes.begin(), orig_sizes.end());
  new_sizes[0] = -1;
  output = output.view(new_sizes);
  output = output.transpose(0, dim);

  Tensor inverse_indices = at::empty({0}, self.type().toScalarType(kLong));
  if (return_inverse) {
    int64_t size = self.size(dim);
    inverse_indices.resize_(size);

    Tensor imask = at::cumsum(mask, 0) - 1;
    for (int i = 0; i < indices.size(); ++i) {
      inverse_indices[indices[i]] = imask[i];
    }
  }

  return std::make_tuple(output, inverse_indices);
}
} // namespace

std::tuple<Tensor, Tensor>
_unique_cpu(const Tensor& self, const bool sorted, const bool return_inverse) {
  return AT_DISPATCH_ALL_TYPES(self.type(), "unique", [&] {
    return _unique_cpu_template<scalar_t>(self, sorted, return_inverse);
  });
}

std::tuple<Tensor, Tensor>
_unique_dim_cpu(const Tensor& self, const int64_t dim, const bool sorted, const bool return_inverse) {
  return AT_DISPATCH_ALL_TYPES(self.type(), "unique_dim", [&] {
    // The current implementation using `dim` always sorts due to unhashable tensors
    return _unique_dim_cpu_template<scalar_t>(self, dim, return_inverse);
  });
}

}  // namespace native
}  // namespace at
