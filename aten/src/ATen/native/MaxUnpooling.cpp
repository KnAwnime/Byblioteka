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
      AT_ERROR(
          "Found an invalid max index",
          error_index,
          " (output volumes are of size ",
          outputHeight,
          "x",
          outputWidth);
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

  // is_empty check
  for (int64_t i = 0; i < self.ndimension(); i++) {
    AT_CHECK(
        self.size(i) > 0,
        "input must be nonempty, but input has sizes: ",
        self.sizes());
  }

  auto numBatch = self.size(0);
  auto numChannels = self.size(1);
  auto outputHeight = output_size[0];
  auto outputWidth = output_size[1];

  output.zero_();
  output.resize_({numBatch, numChannels, outputHeight, outputWidth});

  AT_CHECK(output.is_contiguous(), "output must be contiguous");

  AT_DISPATCH_FLOATING_TYPES(
      self.type(), "MaxUnpooling2d_forward_out_cpu_", ([&] {
        MaxUnpooling2d_forward_out_cpu_<scalar_t>(
            output,
            self.contiguous(),
            indices.contiguous(),
            output_size[0],
            output_size[1]);
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
  for (int64_t i = 0; i < self.ndimension(); i++) {
    AT_CHECK(
        self.size(i) > 0,
        "input must be nonempty, but input has sizes: ",
        self.sizes());
  }
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
        AT_ERROR(
            "found an invalid max index %ld (output volumes are of size %dx%dx%d)",
            error_index,
            oT,
            oH,
            oW);
      }
    }
  }
  return output;
}

void MaxUnpooling3d_shape_check(
    const Tensor& input,
    const Tensor& gradOutput,
    const Tensor& indices,
    IntList output_size,
    IntList stride,
    IntList padding,
    bool check_grad) {
  // is_empty check
  for (int64_t i = 0; i < input.ndimension(); i++) {
    AT_CHECK(
        input.size(i) > 0,
        "input must be nonempty, but input has sizes: ",
        input.sizes());
  }
  AT_CHECK(input.sizes() == indices.sizes());
  AT_CHECK(
      stride[0] > 0 && stride[1] > 0 && stride[2] > 0,
      "stride should be never greater than zero, but got stride: ",
      stride);

  int dimw = 4;
  int dimh = 3;
  int dimt = 2;
  int dimn = 1;

  int nslices = input.size(dimn);
  if (check_grad) {
    if (output_size[0] != gradOutput.size(dimt) ||
        output_size[1] != gradOutput.size(dimh) ||
        output_size[2] != gradOutput.size(dimw)) {
      AT_ERROR(
          "Inconsistent gradOutput size. output_size[1]= %d, output_size[1] = %d, output_size[2] = %d, gradOutput: %dx%dx%d",
          output_size[0],
          output_size[1],
          output_size[2],
          gradOutput[0],
          gradOutput[1],
          gradOutput[2]);
    }
    AT_CHECK(gradOutput.ndimension() == 5 && gradOutput.size(dimn) == nslices);
  }
}

at::Tensor& MaxUnpooling3d_forward_out_cpu(
    Tensor& output,
    const Tensor& self,
    const Tensor& indices,
    IntList output_size,
    IntList stride,
    IntList padding) {
  AT_CHECK(
      output.ndimension() == 5,
      "Output to MaxUnpooling2d should be a NCDHW Tensor",
      output.sizes());
  AT_CHECK(
      self.ndimension() == 5,
      "Input to MaxUnpooling2d should be a NCDHW Tensor",
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
  MaxUnpooling3d_shape_check(
      self, at::empty({}), indices, output_size, stride, padding, false);
  output.zero_();
  output.resize_({self.size(0),
                  self.size(1),
                  output_size[0],
                  output_size[1],
                  output_size[2]});
  AT_DISPATCH_FLOATING_TYPES(
      self.type(), "MaxUnpooling3d_forward_out_cpu_", ([&] {
        MaxUnpooling3d_forward_out_cpu_<scalar_t>(
            output,
            self.contiguous(),
            indices.contiguous(),
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

template <typename scalar_t>
static void MaxUnpooling2d_backward_out_cpu_(
  scalar_t* gradInput_p,
  scalar_t* gradOutput_p,
  int64_t* ind_p,
  int64_t nslices,
  int64_t iwidth, int64_t iheight,
  int64_t owidth, int64_t oheight
)
{
  int k;
#pragma omp parallel for private(k)
  for (k = 0; k < nslices; k++) {
    scalar_t* gradInput_p_k = gradInput_p + k * iwidth * iheight;
    scalar_t* gradOutput_p_k = gradOutput_p + k * owidth * oheight;
    int64_t* ind_p_k = ind_p + k * iwidth * iheight;

    int i, j;
    int64_t maxp;
    for (i = 0; i < iheight; i++) {
      for (j = 0; j < iwidth; j++) {
        maxp = ind_p_k[i * iwidth + j]; /* retrieve position of max */
        if (maxp < 0 || maxp >= owidth * oheight) {
          AT_ERROR(
              "invalid max index ",
              maxp,
              ", owidth= ",
              owidth,
              ", oheight= ",
              oheight);
        }
        gradInput_p_k[i * iwidth + j] =
            gradOutput_p_k[maxp]; /* update gradient */
      }
    }
  }
}

at::Tensor& MaxUnpooling2d_backward_out_cpu(
    Tensor& grad_input,
    const Tensor& grad_output,
    const Tensor& self,
    const Tensor& indices,
    IntList output_size) {
  int dimw = 3;
  int dimh = 2;
  int nbatch = 1;

  AT_CHECK(
      self.sizes() == indices.sizes(),
      "Input shape must match indices shape");

  AT_CHECK(output_size.size() == 2, "Output size must be 2");
  int owidth = output_size[0];
  int oheight = output_size[1];
  auto gradOutput = grad_output.contiguous();
  auto indicesContiguous = indices.contiguous();

  grad_input.resize_as_(self);
  grad_input.zero_();

  int nslices = self.size(dimh - 1);
  int iheight = self.size(dimh);
  int iwidth = self.size(dimw);

  if (output_size[0] != gradOutput.size(dimh) ||
      output_size[1] != gradOutput.size(dimw)) {
    AT_ERROR(
        "Inconsistent gradOutput size",
        output_size[0],
        ", output width= ",
        output_size[1],
        ", gradOutput: ",
        gradOutput.size(dimh),
        "x",
        gradOutput.size(dimw));
  }

  int p;
  for (p = 0; p < nbatch; p++) {
    auto inputOffset = p * nslices * iheight * iwidth;
    auto outputOffset = p * nslices * oheight * owidth;
    AT_DISPATCH_FLOATING_TYPES(
        self.type(), "MaxUnpooling2d_backward_out_cpu_", ([&] {
          MaxUnpooling2d_backward_out_cpu_<scalar_t>(
              grad_input.data<scalar_t>() + inputOffset,
              gradOutput.data<scalar_t>() + outputOffset,
              indices.data<int64_t>() + inputOffset,
              nslices,
              iwidth,
              iheight,
              output_size[0],
              output_size[1]);
        }));
  }
  return grad_input;
}

at::Tensor MaxUnpooling2d_backward_cpu(
    const Tensor& grad_output,
    const Tensor& self,
    const Tensor& indices,
    IntList output_size) {
  auto grad_input = at::zeros_like(self);
  MaxUnpooling2d_backward_out_cpu(
      grad_input, grad_output, self, indices, output_size);
  return grad_input;
}

template <typename scalar_t>
static void MaxUnpooling3d_backward_out_cpu_(
    scalar_t* gradInput_p,
    scalar_t* gradOutput_p,
    int64_t* ind_p,
    int64_t nslices,
    int64_t iT,
    int64_t iW,
    int64_t iH,
    int64_t oT,
    int64_t oW,
    int64_t oH) {
  int k;
#pragma omp parallel for private(k)
  for (k = 0; k < nslices; k++) {
    scalar_t* gradInput_p_k = gradInput_p + k * iT * iH * iW;
    scalar_t* gradOutput_p_k = gradOutput_p + k * oT * oH * oW;
    int64_t* ind_p_k = ind_p + k * iT * iH * iW;

    int t, i, j, index;
    int64_t maxp;
    for (t = 0; t < iT; t++) {
      for (i = 0; i < iH; i++) {
        for (j = 0; j < iW; j++) {
          index = t * iH * iW + i * iW + j;
          maxp = ind_p_k[index]; /* retrieve position of max */
          if (maxp < 0 || maxp >= oT * oH * oW) {
            AT_ERROR(
                "invalid max index ",
                maxp,
                ", oT= ",
                oT,
                ", oW= ",
                oW,
                ",oH= ",
                oH);
          }
          gradInput_p_k[index] = gradOutput_p_k[maxp]; /* update gradient */
        }
      }
    }
  }
}
at::Tensor& MaxUnpooling3d_backward_out_cpu(
    Tensor& grad_input,
    const Tensor& grad_output,
    const Tensor& self,
    const Tensor& indices,
    IntList output_size,
    IntList stride,
    IntList padding) {
  int dimw = 4;
  int dimh = 3;
  int dimt = 2;

  MaxUnpooling3d_shape_check(self, grad_output, indices, output_size, stride, padding, true);

  auto grad_output_contiguous = grad_output.contiguous();
  auto indices_contiguous = indices.contiguous();
  /* resize */
  grad_input.resize_as_(self);
  grad_input.zero_();

  /* sizes */
  auto nbatch = self.size(0);
  auto nslices = self.size(dimt-1);
  auto iT = self.size(dimt);
  auto iH = self.size(dimh);
  auto iW = self.size(dimw);
  auto oT = output_size[0];
  auto oH = output_size[1];
  auto oW = output_size[2];
  /* backprop */
  int p;
  for(p = 0; p < nbatch; p++)
  {
    int inputOffset = p * nslices * iT * iH * iW;
    int outputOffset = p * nslices * oT * oT * oW;
    AT_DISPATCH_FLOATING_TYPES(
        self.type(), "MaxUnpooling3d_backward_out_cpu_", ([&] {
          MaxUnpooling3d_backward_out_cpu_<scalar_t>(
              grad_input.data<scalar_t>() + inputOffset,
              grad_output_contiguous.data<scalar_t>() + outputOffset,
              indices_contiguous.data<int64_t>() + inputOffset,
              nslices,
              iT, iW, iH,
              oT, oW, oH);
        }));
  }
  return grad_input;
}

at::Tensor MaxUnpooling3d_backward_cpu(
  const Tensor& grad_output,
  const Tensor& self,
  const Tensor& indices,
  IntList output_size,
  IntList stride,
  IntList padding) {
    auto grad_input = at::zeros_like(self);
    MaxUnpooling3d_backward_out_cpu(grad_input, grad_output, self, indices, output_size, stride, padding);
    return grad_input;
  }

// stopgap until GPU version is implemented
at::Tensor& MaxUnpooling3d_forward_out_cuda(
    Tensor& output,
    const Tensor& self,
    const Tensor& indices,
    IntList output_size,
    IntList stride,
    IntList padding) {
  return at::_thnn_max_unpool3d_out(
      output, self, indices, output_size, stride, padding);
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

Tensor & MaxUnpooling2d_backward_out_cuda(Tensor & grad_input, const Tensor & grad_output, const Tensor & self, const Tensor & indices, IntList output_size) {
  return at::_thnn_max_unpool2d_backward_out(grad_input, grad_output, self, indices, output_size);
}

Tensor MaxUnpooling2d_backward_cuda(const Tensor & grad_output, const Tensor & self, const Tensor & indices, IntList output_size) {
  return at::_thnn_max_unpool2d_backward(grad_output, self, indices, output_size);
}

Tensor & MaxUnpooling3d_backward_out_cuda(Tensor & grad_input, const Tensor & grad_output, const Tensor & self, const Tensor & indices, IntList output_size, IntList stride, IntList padding) {
  return at::_thnn_max_unpool3d_backward_out(grad_input, grad_output, self, indices, output_size, stride, padding);
}

Tensor MaxUnpooling3d_backward_cuda(const Tensor & grad_output, const Tensor & self, const Tensor & indices, IntList output_size, IntList stride, IntList padding) {
  return at::_thnn_max_unpool3d_backward(grad_output, self, indices, output_size, stride, padding);
}

} // namespace native
} // namespace at
