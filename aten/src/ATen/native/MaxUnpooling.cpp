#include <tuple>
#include "ATen/ATen.h"
#include "ATen/NativeFunctions.h"

namespace at {
namespace native {

template <typename scalar_t>
at::Tensor MaxUnpooling2d_forward_out_cpu_(
    Tensor& output,
    const Tensor& input,
    const Tensor& indices,
    int64_t outputHeight,
    int64_t outputWidth) {
  // TODO: replicate is_empty() cbeck in SpatialMaxUnpooling.c
  // TODO: zero out output inplace

  auto numBatch = input.size(0);
  auto numChannels = input.size(1);
  auto inputHeight = input.size(2);
  auto inputWidth = input.size(3);

  auto* rawInput = input.data<scalar_t>();
  auto* rawIndices = indices.data<int64_t>();
  auto* rawOutput = output.data<scalar_t>();

  for (auto n = 0; n < numBatch; n++) {
    auto nOutputOffset = n * numChannels * outputWidth * outputHeight;
    auto nInputOffset = n * numChannels * inputWidth * inputHeight;
    int k;
    int has_error = 0;
    int error_index = 0;
    #pragma omp parallel for private(k)
    for (k = 0; k < numChannels; k++) {
      auto finalOutputOffset = nOutputOffset + k * outputWidth * outputHeight;
      auto finalInputOffset = nInputOffset + k * inputWidth * inputHeight;
      auto* output_p_k = rawOutput + finalOutputOffset;
      auto* input_p_k = rawInput + finalInputOffset;
      auto* ind_p_k = rawIndices + finalInputOffset;

      int maxp;
      for (auto i = 0; i < inputHeight; i++) {
        for (auto j = 0; j < inputWidth; j++) {
          maxp = ind_p_k[i * inputWidth + j];
          if (maxp < 0 || maxp >= outputWidth * outputHeight) {
    #pragma omp critical
          {
            has_error = 1;
            error_index = maxp;
          }
          } else {
            output_p_k[maxp] = input_p_k[i * inputWidth + j];
          }
        }
      }
    }
    if (has_error) {
      AT_ERROR("Found an invalid max index %ld (output volumes are of size %dx%d)",
        error_index, outputHeight, outputWidth);
    }
  }
  return output;
}

at::Tensor& MaxUnpooling2d_forward_out_cpu(
    Tensor& output,
    const Tensor& self,
    const Tensor& indices,
    IntList output_size) {
  AT_CHECK(
      output_size.size() == 2,
      "There should be exactly two elements (height, width) in output_size");
  AT_CHECK(
      self.ndimension() == 4,
      "Input to MaxUnpooling2d should be a NCHW Tensor");
  AT_CHECK(
      self.sizes() == indices.sizes(),
      "Shape of indices should match shape of input");
  AT_CHECK(self.is_contiguous(), "input must be contiguous");
  AT_CHECK(indices.is_contiguous(), "indices must be contiguous");

  auto numBatch = self.size(0);
  auto numChannels = self.size(1);
  auto outputHeight = output_size[0];
  auto outputWidth = output_size[1];
  AT_CHECK(
      output.sizes() ==
          IntList({numBatch, numChannels, outputHeight, outputWidth}),
      "The first two dimensions of output should match those of input, and last two dimensions should match output_size");
  AT_CHECK(output.is_contiguous(), "output must be contiguous");
  AT_DISPATCH_FLOATING_TYPES(
      self.type(), "MaxUnpooling2d_forward_out_cpu_", ([&] {
        MaxUnpooling2d_forward_out_cpu_<scalar_t>(
            output, self, indices, output_size[0], output_size[1]);
      }));
  return output;
};

at::Tensor MaxUnpooling2d_forward_cpu(
    const Tensor& self,
    const Tensor& indices,
    IntList output_size) {
  AT_CHECK(
      self.ndimension() == 4,
      "Input to MaxUnpooling2d should be a NCHW Tensor",
      self.sizes());
  AT_CHECK(
      output_size.size() == 2,
      "There should be exactly two elements (height, width) in output_size");
  auto output = at::zeros(
      {self.size(0), self.size(1), output_size[0], output_size[1]},
      self.options());
  MaxUnpooling2d_forward_out_cpu(output, self, indices, output_size);
  return output;
};

Tensor MaxUnpooling2d_backward_cpu(
    const Tensor& self,
    const Tensor& indices,
    IntList output_size) {
  AT_ERROR("not implemented");
}

// stopgap until GPU version is implemented
at::Tensor& MaxUnpooling2d_forward_out_cuda(
    Tensor& output,
    const Tensor& self,
    const Tensor& indices,
    IntList output_size) {
  return at::_thnn_max_unpool2d_out(output, self, indices, output_size);
}

// stopgap until GPU version is implemented
at::Tensor MaxUnpooling2d_forward_cuda(
    const Tensor& self,
    const Tensor& indices,
    IntList output_size) {
  return at::_thnn_max_unpool2d(self, indices, output_size);
}

template <typename scalar_t>
at::Tensor MaxUnpooling3d_forward_out_cpu_(
    Tensor& output,
    const Tensor& input,
    const Tensor& indices,
    int64_t oT,
    int64_t oW,
    int64_t oH,
    int64_t dT,
    int64_t dW,
    int64_t dH,
    int64_t pT,
    int64_t pW,
    int64_t pH) {
  auto nBatch = input.size(0);
  auto nSlices = input.size(1);

  auto dimw = 4;
  auto dimh = 3;
  auto dimt = 2;

  auto iT = input.size(dimt);
  auto iH = input.size(dimh);
  auto iW = input.size(dimw);

  // TODO: zero out output inplace
  auto* input_data = input.data<scalar_t>();
  auto* output_data = output.data<scalar_t>();
  auto* indices_data = indices.data<int64_t>();

  for (auto p = 0; p < nBatch; p++) {
    auto inputOffset = p * nSlices * iT * iW * iH;
    auto outputOffset = p * nSlices * oT * oW * oH;
    int k;
    int has_error = 0;
    int error_index = 0;
  #pragma omp parallel for private(k)
    for (k = 0; k < nSlices; k++) {
      auto finalInputOffset = inputOffset + k * iT * iW * iH;
      auto finalOutputOffset = outputOffset + k * oT * oW * oH;

      auto* output_p_k = output_data + finalOutputOffset;
      auto* input_p_k = input_data + finalInputOffset;
      auto* ind_p_k = indices_data + finalInputOffset;
      int maxp;
      for (auto t = 0; t < iT; t++) {
        for (auto i = 0; i < iH; i++) {
          for (auto j = 0; j < iW; j++) {
            auto index = t * iH * iW + i * iW + j;
            maxp = ind_p_k[index];
            if (maxp < 0 || maxp >= oT * oW * oH) {
              #pragma omp critical
              {
                has_error = 1;
                error_index = maxp;
              }
            } else {
              output_p_k[maxp] = input_p_k[index];
            }
          }
        }
      }
      if (has_error) {
        AT_ERROR("found an invalid max index %ld (output volumes are of size %dx%dx%d)",
          error_index, oT, oH, oW);
      }
    }
  }
  return output;
}

at::Tensor& MaxUnpooling3d_forward_out_cpu(
    Tensor& output,
    const Tensor& self,
    const Tensor& indices,
    IntList output_size,
    IntList stride,
    IntList padding) {
  // _thnn_max_unpool3d(Tensor self, LongTensor indices, IntList[3] output_size,
  // IntList[3] stride, IntList[3] padding)
  AT_CHECK(
      output.ndimension() == 5,
      "Output to MaxUnpooling2d should be a NCDHW Tensor",
      output.sizes());
  AT_CHECK(
      self.ndimension() == 5,
      "Output to MaxUnpooling2d should be a NCDHW Tensor",
      self.sizes());
  AT_CHECK(
      output_size.size() == 3,
      "There should be exactly three elements (depth, height, width) in output_size");
  AT_CHECK(
      stride.size() == 3,
      "There should be exactly three elements (depth, height, width) in stide");
  AT_CHECK(
      padding.size() == 3,
      "There should be exactly three elements (depth, height, width) in padding");
  AT_CHECK(
      self.sizes() == indices.sizes(),
      "Shape of indices should match shape of input");
  AT_CHECK(self.is_contiguous(), "input must be contiguous");
  AT_CHECK(indices.is_contiguous(), "indices must be contiguous");
  AT_CHECK(output.is_contiguous(), "output must be contiguous");
  AT_DISPATCH_FLOATING_TYPES(
      self.type(), "MaxUnpooling3d_forward_out_cpu_", ([&] {
        MaxUnpooling3d_forward_out_cpu_<scalar_t>(
            output,
            self,
            indices,
            output_size[0],
            output_size[1],
            output_size[2],
            stride[0],
            stride[1],
            stride[2],
            padding[0],
            padding[1],
            padding[2]);
      }));
  return output;
}

at::Tensor MaxUnpooling3d_forward_cpu(
    const Tensor& self,
    const Tensor& indices,
    IntList output_size,
    IntList stride,
    IntList padding) {
  AT_CHECK(
      self.ndimension() == 5,
      "Input to MaxUnpooling2d should be a NCDHW Tensor",
      self.sizes());
  AT_CHECK(
      output_size.size() == 3,
      "There should be exactly three elements (depth, height, width) in output_size");
  auto output = at::zeros(
      {self.size(0),
       self.size(1),
       output_size[0],
       output_size[1],
       output_size[2]},
      self.options());
  MaxUnpooling3d_forward_out_cpu(
      output, self, indices, output_size, stride, padding);
  return output;
}

// stopgap until GPU version is implemented
at::Tensor& MaxUnpooling3d_forward_out_cuda(
    Tensor& output,
    const Tensor& self,
    const Tensor& indices,
    IntList output_size,
    IntList stride,
    IntList padding) {
  return at::_thnn_max_unpool3d_out(output, self, indices, output_size, stride, padding);
}

// stopgap until GPU version is implemented
at::Tensor MaxUnpooling3d_forward_cuda(
    const Tensor& self,
    const Tensor& indices,
    IntList output_size,
    IntList stride,
    IntList padding) {
  return at::_thnn_max_unpool3d(self, indices, output_size, stride, padding);
}

} // namespace native
} // namespace at
