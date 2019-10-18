#include <ATen/ATen.h>
#include <ATen/AccumulateType.h>
#include <ATen/Dispatch.h>
#include <ATen/Parallel.h>
#include <ATen/TensorUtils.h>

namespace at {
namespace native {

namespace {

void nll_loss_forward_out_cpu_template(
    Tensor& output,
    Tensor& total_weight,
    const Tensor& input,
    const Tensor& target,
    const Tensor& weight,
    int64_t reduction,
    int64_t ignore_index) {
  TORCH_CHECK(
      input.dim() > 0 && input.dim() <= 2, "input tensor should be 1D or 2D");
  TORCH_CHECK(target.dim() == 1, "multi-target not supported");

  const auto n_dims = input.dim();
  const auto n_classes = input.size(-1);

  TORCH_CHECK(
      !weight.defined() || weight.numel() == n_classes,
      "weight tensor should be defined either for all ",
      n_classes,
      " classes or no classes"
      " but got weight tensor of shape: ",
      weight.sizes());

  total_weight.resize_({1});

  AT_DISPATCH_FLOATING_TYPES_AND_HALF(
      input.scalar_type(), "nll_loss_forward_out_cpu_template", [&] {
        auto weight_contiguous = weight.contiguous();
        const scalar_t* weight_data =
            weight.defined() ? weight_contiguous.data_ptr<scalar_t>() : nullptr;

        if (reduction == Reduction::None && n_dims == 2) {
          const auto batch_size = input.size(0);
          output.resize_({batch_size});

          auto input_acc = input.accessor<scalar_t, 2>();
          auto target_acc = target.accessor<int64_t, 1>();
          auto output_acc = output.accessor<scalar_t, 1>();

          std::atomic<int> invalid_target(
              -1); // We cannot throw an exception inside parallel section
          at::parallel_for(0, batch_size, 0, [&](int64_t start, int64_t end) {
            for (auto i = start; i < end; i++) {
              const auto cur_target = target_acc[i];

              if (cur_target == ignore_index) {
                output_acc[i] = 0;
                continue;
              }
              if (cur_target >= 0 && cur_target < n_classes) {
                scalar_t cur_weight = weight_data != nullptr
                    ? weight_data[cur_target]
                    : static_cast<scalar_t>(1);
                output_acc[i] = -input_acc[i][cur_target] * cur_weight;
              } else {
                int tmp = -1;
                invalid_target.compare_exchange_strong(tmp, cur_target);
              }
            }
          });

          TORCH_CHECK(
              invalid_target.load() < 0,
              "Target ",
              invalid_target.load(),
              " out of bounds");

          return;
        }

        // produce scalar output when reducing or input is 1d
        output.resize_({});

        auto input_contiguous = input.contiguous();
        auto target_contiguous = target.contiguous();

        const scalar_t* input_data = input_contiguous.data_ptr<scalar_t>();
        const int64_t* target_data = target_contiguous.data_ptr<int64_t>();

        scalar_t output_val = 0;
        scalar_t total_weight_val = 0;

        if (input.dim() == 1) {
          const auto cur_target = target_data[0];
          if (cur_target != ignore_index) {
            TORCH_CHECK(cur_target >= 0 && cur_target < n_classes);
            total_weight_val = weight_data ? weight_data[cur_target]
                                           : static_cast<scalar_t>(1);
            output_val = -input_data[cur_target] * total_weight_val;
          }
        } else if (input.dim() == 2) {
          const auto batch_size = input.size(0);
          TORCH_CHECK(target.size(0) == batch_size);
          const auto n_target = input.size(1);

          for (int64_t i = 0; i < batch_size; i++) {
            const auto cur_target = target_data[i];
            if (cur_target != ignore_index) {
              TORCH_CHECK(cur_target >= 0 && cur_target < n_classes);

              scalar_t cur_weight = weight_data ? weight_data[cur_target]
                                                : static_cast<scalar_t>(1);
              total_weight_val += cur_weight;
              output_val -= input_data[i * n_target + cur_target] * cur_weight;
            }
          }
        }

        if (reduction == Reduction::Mean &&
            (total_weight_val != 0 || input.numel() == 0)) {
          // allow NaN result for total_weight_val == 0 case, see #15870
          output_val /= total_weight_val;
        }

        // write result to output tensors
        auto total_weight_acc = total_weight.accessor<scalar_t, 1>();
        *output.data_ptr<scalar_t>() = output_val;
        total_weight_acc[0] = total_weight_val;
      });
}

void nll_loss_backward_out_cpu_template(
    Tensor& grad_input,
    const Tensor& grad_output,
    const Tensor& input,
    const Tensor& target,
    const Tensor& weight,
    int64_t reduction,
    int64_t ignore_index,
    const Tensor& total_weight) {
  TORCH_CHECK(target.dim() == 1, "multi-target not supported");
  TORCH_CHECK(input.dim() <= 2, "input tensor should be 1D or 2D");

  grad_input.resize_as_(input);
  grad_input.zero_();

  const auto n_dims = input.dim();
  const auto n_classes = input.size(-1);

  TORCH_CHECK(grad_input.is_contiguous(), "grad_input must be contiguous");
  TORCH_CHECK(
      !weight.defined() || weight.numel() == n_classes,
      "weight tensor should be defined either for all or no classes");

  AT_DISPATCH_FLOATING_TYPES_AND_HALF(
      input.scalar_type(), "nll_loss_backward_out_cpu_template", [&] {
        auto target_acc = target.accessor<int64_t, 1>();

        auto weight_contiguous = weight.contiguous();
        const scalar_t* weight_data =
            weight.defined() ? weight_contiguous.data_ptr<scalar_t>() : nullptr;

        if (reduction == Reduction::None && n_dims == 2) {
          auto grad_input_acc = grad_input.accessor<scalar_t, 2>();
          auto grad_output_acc = grad_output.accessor<scalar_t, 1>();

          const auto batch_size = input.size(0);
          check_dim_size(grad_output, 1, 0, batch_size);
          at::parallel_for(0, batch_size, 0, [&](int64_t start, int64_t end) {
            for (auto i = start; i < end; i++) {
              auto cur_target = target_acc[i];
              if (cur_target == ignore_index) {
                continue;
              }
              const scalar_t w = weight_data ? weight_data[cur_target]
                                             : static_cast<scalar_t>(1);
              grad_input_acc[i][cur_target] = -w * grad_output_acc[i];
            }
          });
          return;
        }

        const scalar_t total_weight_value =
            total_weight.accessor<scalar_t, 1>()[0];
        if (total_weight_value <= 0) {
          return;
        }

        TORCH_CHECK(
            grad_output.dim() <= 1 && grad_output.numel() == 1,
            "Expected a single element grad_output tensor, but got: ",
            grad_output.sizes());
        const scalar_t grad_output_value = *grad_output.data_ptr<scalar_t>();

        if (input.dim() == 1) {
          auto grad_input_acc = grad_input.accessor<scalar_t, 1>();

          const auto cur_target = target_acc[0];
          if (cur_target != ignore_index) {
            TORCH_CHECK(cur_target >= 0 && cur_target < n_classes);

            grad_input_acc[cur_target] =
                (reduction != Reduction::Mean && weight_data != nullptr)
                ? -weight_data[cur_target]
                : static_cast<scalar_t>(-1);
            grad_input_acc[cur_target] *= grad_output_value;
          }
        } else if (input.dim() == 2) {
          auto grad_input_acc = grad_input.accessor<scalar_t, 2>();

          const auto batch_size = input.size(0);
          TORCH_CHECK(target.size(0) == batch_size);

          for (int64_t i = 0; i < batch_size; i++) {
            const auto cur_target = target_acc[i];

            if (cur_target != ignore_index) {
              TORCH_CHECK(cur_target >= 0 && cur_target < n_classes);

              const scalar_t w = weight_data != nullptr
                  ? weight_data[cur_target]
                  : static_cast<scalar_t>(1);
              grad_input_acc[i][cur_target] = -w * grad_output_value;

              if (reduction == Reduction::Mean) {
                grad_input_acc[i][cur_target] /= total_weight_value;
              }
            }
          }
        }
      });
}

} // namespace

std::tuple<Tensor&, Tensor&> nll_loss_forward_out_cpu(
    Tensor& output,
    Tensor& total_weight,
    const Tensor& self,
    const Tensor& target,
    const Tensor& weight,
    int64_t reduction,
    int64_t ignore_index) {
  nll_loss_forward_out_cpu_template(
      output, total_weight, self, target, weight, reduction, ignore_index);
  return std::tuple<Tensor&, Tensor&>(output, total_weight);
}

std::tuple<Tensor, Tensor> nll_loss_forward_cpu(
    const Tensor& self,
    const Tensor& target,
    const Tensor& weight,
    int64_t reduction,
    int64_t ignore_index) {
  auto output = at::empty({0}, self.options());
  auto total_weight = at::empty({0}, self.options());
  nll_loss_forward_out_cpu(
      output, total_weight, self, target, weight, reduction, ignore_index);
  return std::make_tuple(output, total_weight);
}

Tensor& nll_loss_backward_out_cpu(
    Tensor& grad_input,
    const Tensor& grad_output,
    const Tensor& self,
    const Tensor& target,
    const Tensor& weight,
    int64_t reduction,
    int64_t ignore_index,
    const Tensor& total_weight) {
  nll_loss_backward_out_cpu_template(
      grad_input,
      grad_output,
      self,
      target,
      weight,
      reduction,
      ignore_index,
      total_weight);
  return grad_input;
}

Tensor nll_loss_backward_cpu(
    const Tensor& grad_output,
    const Tensor& self,
    const Tensor& target,
    const Tensor& weight,
    int64_t reduction,
    int64_t ignore_index,
    const Tensor& total_weight) {
  auto grad_input = at::zeros_like(self);
  nll_loss_backward_out_cpu(
      grad_input,
      grad_output,
      self,
      target,
      weight,
      reduction,
      ignore_index,
      total_weight);
  return grad_input;
}

} // namespace native
} // namespace at
