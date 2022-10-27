#include <torch/csrc/lazy/ts_backend/tensor_aten_ops.h>

#include <ATen/InferSize.h>
#include <c10/util/Optional.h>
#include <c10/util/irange.h>
#include <torch/csrc/autograd/variable.h>
#include <torch/csrc/lazy/core/helpers.h>
#include <torch/csrc/lazy/core/ir_builder.h>
#include <torch/csrc/lazy/core/ir_util.h>
#include <torch/csrc/lazy/core/lazy_graph_executor.h>
#include <torch/csrc/lazy/core/metrics.h>
#include <torch/csrc/lazy/core/ops/arithmetic_ir_ops.h>
#include <torch/csrc/lazy/core/ops/utils.h>
#include <torch/csrc/lazy/core/tensor.h>
#include <torch/csrc/lazy/core/util.h>
#include <torch/csrc/lazy/generated/LazyIr.h>
#include <torch/csrc/lazy/ts_backend/ops/random_ops.h>
#include <algorithm>
#include <functional>

namespace torch {
namespace lazy {
namespace {

// to enable operator+-*/ for Value
using namespace torch::lazy;

torch::lazy::Value MaybeExpand(
    const torch::lazy::Value& input,
    const torch::lazy::Shape& target_shape) {
  if (input.shape().sizes() == target_shape.sizes()) {
    return input;
  }
  return torch::lazy::MakeExpand(
      input,
      target_shape.sizes().vec(),
      /*is_scalar_expand=*/false);
}

std::vector<int64_t> GetExpandDimensions(
    const torch::lazy::Shape& shape,
    std::vector<int64_t> dimensions) {
  TORCH_CHECK_GE(dimensions.size(), shape.dim()) << shape;
  int64_t base = dimensions.size() - shape.dim();
  for (const auto i : c10::irange(shape.dim())) {
    if (dimensions[base + i] == -1) {
      dimensions[base + i] = shape.size(i);
    }
  }
  return dimensions;
}

torch::lazy::ViewInfo CreateAsStridedViewInfo(
    const torch::lazy::Shape& input_shape,
    std::vector<int64_t> size,
    std::vector<int64_t> stride,
    c10::optional<int64_t> storage_offset) {
  torch::lazy::Shape result_shape =
      torch::lazy::Shape(input_shape.scalar_type(), size);
  torch::lazy::AsStridedInfo as_strided_info;
  as_strided_info.stride = std::move(stride);
  if (storage_offset) {
    as_strided_info.offset = *storage_offset;
  }
  return torch::lazy::ViewInfo(
      torch::lazy::ViewInfo::Type::kAsStrided,
      std::move(result_shape),
      input_shape,
      std::move(as_strided_info));
}

} // namespace

//////////////////////////////////////////////////////////////////////////////
// ATEN operators follows here, listed in alphabetical order.
//////////////////////////////////////////////////////////////////////////////
torch::lazy::LazyTensorPtr as_strided(
    const torch::lazy::LazyTensorPtr& input,
    std::vector<int64_t> size,
    std::vector<int64_t> stride,
    c10::optional<int64_t> storage_offset) {
  auto input_shape = input->shape();
  return input->CreateViewTensor(CreateAsStridedViewInfo(
      input_shape, std::move(size), std::move(stride), storage_offset));
}

void as_strided_(
    torch::lazy::LazyTensorPtr& input,
    std::vector<int64_t> size,
    std::vector<int64_t> stride,
    c10::optional<int64_t> storage_offset) {
  if (input->data()->view == nullptr) {
    input->SetIrValue(torch::lazy::MakeAsStrided(
        input->GetIrValue(),
        std::move(size),
        std::move(stride),
        storage_offset.value_or(0)));
  } else {
    auto input_shape = input->shape();
    input->SetSubView(CreateAsStridedViewInfo(
        input_shape, std::move(size), std::move(stride), storage_offset));
  }
}

torch::lazy::LazyTensorPtr expand(
    const torch::lazy::LazyTensorPtr& input,
    std::vector<int64_t> size) {
  auto input_shape = input->shape();
  return torch::lazy::LazyTensor::Create(
      torch::lazy::MakeExpand(
          input->GetIrValue(),
          GetExpandDimensions(input_shape.Get(), std::move(size)),
          /*is_scalar_expand=*/false),
      input->GetDevice());
}

void fill_(torch::lazy::LazyTensorPtr& input, const at::Scalar& value) {
  torch::lazy::Value constant =
      torch::lazy::LazyGraphExecutor::Get()->GetIrValueForExpandedScalar(
          value, input->shape(), input->GetDevice());
  input->SetInPlaceIrValue(std::move(constant));
}

torch::lazy::LazyTensorPtr narrow(
    const torch::lazy::LazyTensorPtr& input,
    int64_t dim,
    int64_t start,
    int64_t length) {
  auto input_shape = input->shape();
  dim = torch::lazy::GetCanonicalDimensionIndex(dim, input_shape.Get().dim());
  torch::lazy::Shape narrow_shape = input_shape;
  narrow_shape.set_size(dim, length);

  torch::lazy::ViewInfo::Type view_type =
      (input_shape.Get().numel() == narrow_shape.numel())
      ? torch::lazy::ViewInfo::Type::kReshape
      : torch::lazy::ViewInfo::Type::kNarrow;
  torch::lazy::ViewInfo view_info(
      view_type, std::move(narrow_shape), input_shape);
  view_info.indices[dim] =
      torch::lazy::GetCanonicalPosition(input_shape.Get().sizes(), dim, start);
  return input->CreateViewTensor(std::move(view_info));
}

torch::lazy::LazyTensorPtr permute(
    const torch::lazy::LazyTensorPtr& input,
    c10::ArrayRef<int64_t> dims) {
  auto input_shape = input->shape();
  torch::lazy::ViewInfo view_info(
      torch::lazy::ViewInfo::Type::kPermute,
      input_shape,
      torch::lazy::GetCanonicalDimensionIndices(dims, input_shape.Get().dim()));
  return input->CreateViewTensor(std::move(view_info));
}

void copy_(torch::lazy::LazyTensorPtr& input, torch::lazy::LazyTensorPtr& src) {
  if (input->GetDevice() == src->GetDevice()) {
    torch::lazy::Value copy_value;
    if (input->dtype() == src->dtype()) {
      copy_value = src->GetIrValue();
    } else {
      copy_value = torch::lazy::MakeCast(
          src->GetIrValue(), input->dtype(), src->dtype());
    }
    input->SetIrValue(MaybeExpand(copy_value, input->shape()));
  } else {
    auto input_shape = input->shape();
    at::Tensor src_tensor = src->ToTensor(/*detached=*/true);
    if (src_tensor.sizes() != input_shape.Get().sizes()) {
      src_tensor = src_tensor.expand(input_shape.Get().sizes().vec());
    }
    input->UpdateFromTensor(std::move(src_tensor), /*sync=*/false);
  }
}

torch::lazy::LazyTensorPtr select(
    const torch::lazy::LazyTensorPtr& input,
    int64_t dim,
    int64_t index) {
  auto shape = input->shape();
  dim = torch::lazy::GetCanonicalDimensionIndex(dim, shape.Get().dim());
  torch::lazy::LazyTensorPtr result = narrow(input, dim, index, 1);
  auto new_dims = torch::lazy::DropDimensions(shape.Get().sizes(), {dim});
  return view(result, new_dims);
}

torch::lazy::LazyTensorPtr slice(
    const torch::lazy::LazyTensorPtr& input,
    int64_t dim,
    int64_t start,
    int64_t end,
    int64_t step) {
  auto input_shape = input->shape();
  dim = torch::lazy::GetCanonicalDimensionIndex(dim, input_shape.Get().dim());
  start =
      torch::lazy::GetCanonicalPosition(input_shape.Get().sizes(), dim, start);
  end = torch::lazy::GetCanonicalPosition(input_shape.Get().sizes(), dim, end);
  // PyTorch allows tensor[-1:0] to return a 0-dim tensor.
  if (start > end) {
    end = start;
  }
  step = std::min(step, end - start);

  torch::lazy::SelectInfo select = {dim, start, end, step};
  torch::lazy::ViewInfo view_info(
      torch::lazy::ViewInfo::Type::kSelect, input_shape, select);
  return input->CreateViewTensor(std::move(view_info));
}

torch::lazy::LazyTensorPtr squeeze(const torch::lazy::LazyTensorPtr& input) {
  auto input_shape = input->shape();
  auto output_dimensions =
      BuildSqueezedDimensions(input_shape.Get().sizes(), /*squeeze_dim=*/-1);
  return view(input, output_dimensions);
}

torch::lazy::LazyTensorPtr squeeze(
    const torch::lazy::LazyTensorPtr& input,
    int64_t dim) {
  auto input_shape = input->shape();
  int64_t squeeze_dim =
      torch::lazy::GetCanonicalDimensionIndex(dim, input->shape().Get().dim());
  auto output_dimensions =
      BuildSqueezedDimensions(input_shape.Get().sizes(), squeeze_dim);
  return view(input, output_dimensions);
}

void squeeze_(torch::lazy::LazyTensorPtr& input) {
  input->SetIrValue(torch::lazy::MakeSqueeze(input->GetIrValue(), -1));
}

void squeeze_(torch::lazy::LazyTensorPtr& input, int64_t dim) {
  input->SetIrValue(torch::lazy::MakeSqueeze(
      input->GetIrValue(),
      torch::lazy::GetCanonicalDimensionIndex(
          dim, input->shape().Get().dim())));
}

torch::lazy::LazyTensorPtr transpose(
    const torch::lazy::LazyTensorPtr& input,
    int64_t dim0,
    int64_t dim1) {
  auto input_shape = input->shape();
  auto permute_dims = torch::lazy::MakeTransposePermutation(
      /*dim0=*/dim0, /*dim1=*/dim1, /*rank=*/input_shape.Get().dim());
  torch::lazy::ViewInfo view_info(
      torch::lazy::ViewInfo::Type::kPermute, input_shape, permute_dims);
  return input->CreateViewTensor(std::move(view_info));
}

void transpose_(torch::lazy::LazyTensorPtr& input, int64_t dim0, int64_t dim1) {
  auto input_shape = input->shape();
  auto permute_dims = torch::lazy::MakeTransposePermutation(
      /*dim0=*/dim0, /*dim1=*/dim1, /*rank=*/input_shape.Get().dim());
  torch::lazy::ViewInfo view_info(
      torch::lazy::ViewInfo::Type::kPermute, input_shape, permute_dims);
  return input->ModifyCurrentView(std::move(view_info));
}

torch::lazy::LazyTensorPtr unsqueeze(
    const torch::lazy::LazyTensorPtr& input,
    int64_t dim) {
  auto input_shape = input->shape();
  int64_t squeeze_dim =
      torch::lazy::GetCanonicalDimensionIndex(dim, input_shape.Get().dim() + 1);
  auto dimensions =
      BuildUnsqueezedDimensions(input_shape.Get().sizes(), squeeze_dim);
  return view(input, dimensions);
}

void unsqueeze_(torch::lazy::LazyTensorPtr& input, int64_t dim) {
  int squeeze_dim = torch::lazy::GetCanonicalDimensionIndex(
      dim, input->shape().Get().dim() + 1);
  input->SetIrValue(
      torch::lazy::MakeUnsqueeze(input->GetIrValue(), squeeze_dim));
}

torch::lazy::LazyTensorPtr view(
    const torch::lazy::LazyTensorPtr& input,
    c10::ArrayRef<int64_t> output_size) {
  auto input_shape = input->shape().Get();
  torch::lazy::Shape shape = torch::lazy::Shape(
      input_shape.scalar_type(),
      at::infer_size(output_size, input_shape.numel()));
  torch::lazy::ViewInfo view_info(
      torch::lazy::ViewInfo::Type::kReshape, std::move(shape), input_shape);
  return input->CreateViewTensor(std::move(view_info));
}

} // namespace lazy
} // namespace torch
