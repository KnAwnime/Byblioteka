#include <ATen/ATen.h>
#include <ATen/Parallel.h>
#include <ATen/NativeFunctions.h>
#include <tuple>

#pragma once

namespace at {
namespace native {

namespace {

template <typename dest_t, typename src_t>
static inline dest_t
safe_downcast(src_t v)
{
  TORCH_CHECK(std::numeric_limits<dest_t>::min() <= v && v <= std::numeric_limits<dest_t>::max(),
              "integer out of range");

  return static_cast<dest_t>(v);
}

template<typename T>
static inline T pooling_output_shape(
        T inputSize, T kernelSize, T pad, T stride, T dilation, bool ceil_mode) {
    T outputSize = ((inputSize + 2 * pad - dilation * (kernelSize - 1) - 1 + (ceil_mode ? stride - 1 : 0)) / stride + 1);
    if (pad) {
        // ensure that the last pooling starts inside the image
        // needed to avoid problems in ceil mode
        if ((outputSize - 1) * stride >= inputSize + pad)
          --outputSize;
    }
    return outputSize;
}

static inline void
max_pool2d_with_indices_shape_check(
  const Tensor& input,
  int kH, int kW, int dH, int dW, int padH, int padW, int dilationH, int dilationW,
  int64_t nInputPlane,
  int64_t inputHeight, int64_t inputWidth,
  int64_t outputHeight, int64_t outputWidth)
{
  const int64_t ndim = input.ndimension();
  const int64_t nOutputPlane = nInputPlane;

  TORCH_CHECK(kW > 0 && kH > 0,
              "kernel size should be greater than zero, but got ",
              "kH: ", kH, " kW: ", kW);
  TORCH_CHECK(dW > 0 && dH > 0,
              "stride should be greater than zero, but got "
              "dH: ", dH, " dW: ", dW);
  TORCH_CHECK(dilationH > 0 && dilationW > 0,
              "dilation should be greater than zero, but got ",
              "dilationH: ", dilationH, " dilationW: ", dilationW);

  TORCH_CHECK(input.numel() > 0 && (ndim == 3 || ndim == 4),
              "non-empty 3D or 4D input tensor expected but got ndim: ", ndim);
  TORCH_CHECK(kW/2 >= padW && kH/2 >= padH,
              "pad should be smaller than half of kernel size, but got ",
              "padW = ", padW, ", padH = ", padH, ", kW = ", kW, ", kH = ", kH);

  if (outputWidth < 1 || outputHeight < 1) {
    AT_ERROR("Given input size: (",
              nInputPlane, "x", inputHeight, "x", inputWidth, "). ",
             "Calculated output size: (",
              nOutputPlane, "x", outputHeight, "x", outputWidth, "). ",
             "Output size is too small");
  }
}

static inline void
max_pool2d_with_indices_shape_check(
  const Tensor& input,
  const Tensor& gradOutput,
  const Tensor& indices,
  int64_t nbatch,
  int kH, int kW, int dH, int dW, int padH, int padW, int dilationH, int dilationW,
  int64_t nInputPlane,
  int64_t inputHeight, int64_t inputWidth,
  int64_t outputHeight, int64_t outputWidth,
  bool cuda=false)
{
  max_pool2d_with_indices_shape_check(
    input,
    kH, kW, dH, dW, padH, padW, dilationH, dilationW,
    nInputPlane, inputHeight, inputWidth, outputHeight, outputWidth);

  const int64_t ndim = input.ndimension();
  const int64_t nOutputPlane = nInputPlane;

  check_dim_size(gradOutput, ndim, ndim-3, nOutputPlane);
  check_dim_size(gradOutput, ndim, ndim-2, outputHeight);
  check_dim_size(gradOutput, ndim, ndim-1, outputWidth);

  if (cuda) {
    check_dim_size(indices, 4, 0, nbatch);
    check_dim_size(indices, 4, 1, nOutputPlane);
    check_dim_size(indices, 4, 2, outputHeight);
    check_dim_size(indices, 4, 3, outputWidth);
  }
  else {
    check_dim_size(indices, ndim, ndim-3, nOutputPlane);
    check_dim_size(indices, ndim, ndim-2, outputHeight);
    check_dim_size(indices, ndim, ndim-1, outputWidth);
  }
}

static inline void
max_pool3d_with_indices_shape_check(
  const Tensor& input,
  int64_t nslices,
  int kT, int kH, int kW,
  int dT, int dH, int dW,
  int pT, int pH, int pW,
  int dilationT, int dilationH, int dilationW,
  int64_t itime, int64_t iheight, int64_t iwidth,
  int64_t otime, int64_t oheight, int64_t owidth)
{
  const int64_t ndim = input.ndimension();

  TORCH_CHECK(kT > 0 && kW > 0 && kH > 0,
              "kernel size should be greater than zero, but got ",
              "kT: ", kT, " kH: ", kH, " kW: ", kW);
  TORCH_CHECK(dT > 0 && dW > 0 && dH > 0,
              "stride should be greater than zero, but got ",
              "dT: ", dT, " dH: ", dH, " dW: ", dW);
  TORCH_CHECK(dilationT > 0 && dilationW > 0 && dilationH > 0,
              "dilation should be greater than zero, but got ",
              "dilationT: ", dilationT, " dilationH: ", dilationH, " dilationW: ", dilationW);

  TORCH_CHECK(input.numel() > 0 && (ndim == 4 || ndim == 5),
              "non-empty 4D or 5D (batch mode) tensor expected for input, but got ndim: ", ndim);

  TORCH_CHECK(kT/2 >= pT && kW/2 >= pW && kH/2 >= pH,
              "pad should be smaller than half of kernel size, but got "
              "kT: ", kT, " kW: ", kW, " kH: ", kH, " padT: ", pT, " padW: ", pW, " padH: ", pH);

  TORCH_CHECK(otime >= 1 && owidth >= 1 && oheight >= 1,
              "Given input size: (",
              nslices,"x", itime, "x", iheight, "x", iwidth, "). ",
              "Calculated output size: (",
              nslices, "x", otime, "x", oheight, "x", owidth, "). ",
              "Output size is too small");
}

static inline void
max_pool3d_with_indices_shape_check(
  const Tensor& input,
  const Tensor& gradOutput,
  const Tensor& indices,
  int64_t nslices,
  int kT, int kH, int kW,
  int dT, int dH, int dW,
  int pT, int pH, int pW,
  int dilationT, int dilationH, int dilationW,
  int64_t itime, int64_t iheight, int64_t iwidth,
  int64_t otime, int64_t oheight, int64_t owidth)
{
  const int64_t ndim = input.ndimension();

  max_pool3d_with_indices_shape_check(
    input,
    nslices,
    kT, kH, kW,
    dT, dH, dW,
    pT, pH, pW,
    dilationT, dilationH, dilationW,
    itime, iheight, iwidth,
    otime, oheight, owidth);

  check_dim_size(gradOutput, ndim, ndim-4, nslices);
  check_dim_size(gradOutput, ndim, ndim-3, otime);
  check_dim_size(gradOutput, ndim, ndim-2, oheight);
  check_dim_size(gradOutput, ndim, ndim-1, owidth);

  check_dim_size(indices, ndim, ndim-4, nslices);
  check_dim_size(indices, ndim, ndim-3, otime);
  check_dim_size(indices, ndim, ndim-2, oheight);
  check_dim_size(indices, ndim, ndim-1, owidth);
}

} // namespace

} // at::native
} // at
