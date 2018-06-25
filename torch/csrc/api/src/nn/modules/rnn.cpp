#include <torch/nn/modules/rnn.h>

#include <torch/nn/modules/dropout.h>
#include <torch/tensor.h>
#include <torch/tensor_list_view.h>
#include <torch/utils.h>

#include <ATen/Error.h>
#include <ATen/optional.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace torch {
namespace nn {
namespace {
Tensor linear(Tensor x, Tensor w, Tensor b) {
  if (x.ndimension() == 2 && b.defined()) {
    // Fused op is marginally faster
    assert(x.size(1) == w.size(1));
    return at::addmm(b, x, w.t());
  }

  auto output = x.matmul(w.t());
  if (b.defined()) {
    output += b;
  }
  return output;
}
} // namespace

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~ RNNOptionsBase ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

namespace detail {
RNNOptionsBase::RNNOptionsBase(int64_t input_size, int64_t hidden_size)
    : input_size_(input_size), hidden_size_(hidden_size) {}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ RNNImplBase ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

template <typename Derived>
RNNImplBase<Derived>::RNNImplBase(
    RNNOptionsBase options,
    at::optional<CuDNNMode> cudnn_mode,
    int64_t number_of_gates,
    bool has_cell_state)
    : options_(options),
      number_of_gates_(number_of_gates),
      has_cell_state_(has_cell_state),
      cudnn_mode_(cudnn_mode) {
  reset();
}

template <typename Derived>
void RNNImplBase<Derived>::reset() {
  if (options_.dropout_ > 0.0) {
    dropout_module_ = Dropout(options_.dropout_);
  }

  ihw_.resize(options_.layers_);
  hhw_.resize(options_.layers_);
  ihb_.resize(options_.layers_);
  hhb_.resize(options_.layers_);

  const int64_t gate_size = options_.hidden_size_ * number_of_gates_;

  for (int64_t layer = 0; layer < options_.layers_; ++layer) {
    const int64_t input_size =
        (layer == 0) ? options_.input_size_ : options_.hidden_size_;
    ihw_[layer] = this->register_parameter(
        "weight_ih_l" + std::to_string(layer),
        torch::empty({gate_size, input_size}));
    hhw_[layer] = this->register_parameter(
        "weight_hh_l" + std::to_string(layer),
        torch::empty({gate_size, options_.hidden_size_}));

    if (options_.with_bias_) {
      ihb_[layer] = this->register_parameter(
          "bias_ih_l" + std::to_string(layer), torch::empty({gate_size}));
      hhb_[layer] = this->register_parameter(
          "bias_hh_l" + std::to_string(layer), torch::empty({gate_size}));
    }
  }

  const auto stdv = 1.0 / std::sqrt(options_.hidden_size_);
  for (auto& p : this->parameters()) {
    p->data().uniform_(-stdv, stdv);
  }
}

RNNOutput RNNImplBase<Derived>::forward(Tensor input, Tensor state) {
  if (cudnn_mode_.has_value() && at::cudnn_is_acceptable(input) &&
      options_.dropout_ == 0) {
    return CUDNN_forward(input, state);
  } else {
    return autograd_forward(input, state);
  }
}

template <typename Derived>
std::vector<Tensor> RNNImplBase<Derived>::flat_weights() const {
  std::vector<Tensor> flat;
  for (int64_t layer = 0; layer < options_.layers_; layer++) {
    flat.push_back(ihw_[layer]);
    flat.push_back(hhw_[layer]);
    if (options_.with_bias_) {
      flat.push_back(ihb_[layer]);
      flat.push_back(hhb_[layer]);
    }
  }
  return flat;
}

template <typename Derived>
RNNOutput RNNImplBase<Derived>::autograd_forward(Tensor input, Tensor state) {
  std::vector<at::Tensor> new_state;
  auto has_hidden = state.defined();
  auto layer_dimension = has_hidden ? state.ndimension() - 3 : -1;
  for (int64_t layer = 0; layer < options_.layers_; layer++) {
    new_state.push_back(
        has_hidden ? state.select(layer_dimension, layer) : Tensor());
  }

  auto output = torch::zeros(
      {input.size(0), input.size(1), options_.hidden_size_}, input.options());
  for (int64_t t = 0; t < input.size(0); t++) {
    auto x = input.select(0, t);
    for (int64_t i = 0; i < options_.layers_; i++) {
      // cell_forward() returns a stacked tensor of one or more cell states.
      auto layer_output = cell_forward(x, new_state[i], i);
      // If there are multiple cell states, keep all. If there is only one,
      // the first dimension will be 1, so `.squeeze(0)` will unpack it.
      new_state[i] = layer_output.squeeze(0);
      // x should always be the hidden cell state h, assumed to be the zero-th.
      x = layer_output[0];
      output.select(0, t).copy_(x);
      if (options_.dropout_ > 0 && i != options_.layers_ - 1) {
        x = dropout_module_->forward(x);
      }
    }
  }

  auto state_output = at::stack(new_state);
  if (has_cell_state_) {
    state_output.transpose_(0, 1);
  }
  return {output, state_output};
}

template <typename Derived>
void RNNImplBase<Derived>::flatten_parameters_for_cudnn() {
  data_ptrs_.clear();
  const auto any_parameter = ihw_.at(0);
  if (!cudnn_mode_.has_value() || !any_parameter.is_cuda() ||
      !at::cudnn_is_acceptable(any_parameter) || options_.dropout_ > 0) {
    return;
  }
  std::unordered_set<void*> unique_data_ptrs;
  auto params = this->parameters();
  for (auto& p : params) {
    unique_data_ptrs.insert(p->data().data_ptr());
  }
  // TODO PyTorch says: If any parameters alias, we fall back to the slower,
  // copying code path. This is a sufficient check, because overlapping
  // parameter buffers that don't completely alias would break the assumptions
  // of the uniqueness check in Module.named_parameters(). But I'm not sure if
  // this is the case for us
  if (unique_data_ptrs.size() != params.size()) {
    return;
  }

  {
    NoGradGuard guard;
    flat_weights_ = at::_cudnn_rnn_flatten_weight(
        TensorListView(flat_weights()),
        /*weight_stride=*/options_.with_bias_ ? 4 : 2,
        options_.input_size_,
        static_cast<int64_t>(*cudnn_mode_),
        options_.hidden_size_,
        options_.layers_,
        /*batch_first=*/false,
        /*bidirectional=*/false);
  }
  for (auto& p : params) {
    data_ptrs_.emplace_back(p->data().data_ptr());
  }
}

template <typename Derived>
RNNOutput RNNImplBase<Derived>::CUDNN_forward(Tensor input, Tensor state) {
  Tensor hx, cx;
  if (state.defined()) {
    if (has_cell_state_) {
      hx = state[0];
      cx = state[1];
    } else {
      hx = state;
    }
  } else {
    hx = torch::zeros(
        {options_.layers_, input.size(1), options_.hidden_size_},
        input.options());
    if (has_cell_state_) {
      cx = torch::zeros(
          {options_.layers_, input.size(1), options_.hidden_size_},
          input.options());
    }
  }
  auto dropout_state = torch::empty({}, input.type());

  std::vector<void*> weight_data_ptrs;
  for (auto& p : this->parameters()) {
    weight_data_ptrs.emplace_back(p->data().data_ptr());
  }

  AT_CHECK(
      weight_data_ptrs == data_ptrs_,
      "Parameters are unflattened! Code path might be super slow. "
      "Please call flatten_parameters_for_cudnn() when you muck "
      "around with storages!")
  AT_CHECK(cudnn_mode_.has_value(), "No CuDNN mode has been supplied!");

  // tup = std::tuple of output, hy, cy, reserve, new_weight_buf
  auto tup = _cudnn_rnn(
      input,
      TensorListView(flat_weights()),
      /*weight_stride=*/options_.with_bias_ ? 4 : 2,
      flat_weights_,
      hx,
      cx,
      static_cast<int64_t>(*cudnn_mode_),
      options_.hidden_size_,
      options_.layers_,
      /*batch_first=*/false,
      0, // TODO Use C++ dropout descriptors
      this->is_training(),
      /*bidirectional=*/false,
      /*packed=*/{},
      dropout_state // TODO waiting on dropout state descriptor in C++ pytorch
  );

  Tensor hidden_output;
  if (has_cell_state_) {
    hidden_output =
        at::stack(TensorListView({std::get<1>(tup), std::get<2>(tup)}), 0);
  } else {
    hidden_output = std::get<1>(tup);
  }

  Tensor output = std::get<0>(tup);
  return {output, hidden_output};
}

template <typename Derived>
void RNNImplBase<Derived>::to(
    at::Device device,
    at::ScalarType dtype,
    bool non_blocking) {
  nn::Module::to(device, dtype, non_blocking);
  flatten_parameters_for_cudnn();
}

template <typename Derived>
void RNNImplBase<Derived>::to(at::ScalarType dtype, bool non_blocking) {
  nn::Module::to(dtype, non_blocking);
  flatten_parameters_for_cudnn();
}

template <typename Derived>
void RNNImplBase<Derived>::to(at::Device device, bool non_blocking) {
  nn::Module::to(device, non_blocking);
  flatten_parameters_for_cudnn();
}

template class RNNImplBase<LSTMImpl>;
template class RNNImplBase<GRUImpl>;
template class RNNImplBase<RNNImpl>;
} // namespace detail

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ RNN ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

RNNOptions::RNNOptions(int64_t input_size, int64_t hidden_size)
    : input_size_(input_size), hidden_size_(hidden_size) {}

RNNOptions& RNNOptions::tanh() {
  return activation(RNNActivation::Tanh);
}

RNNOptions& RNNOptions::relu() {
  return activation(RNNActivation::ReLU);
}

RNNImpl::RNNImpl(RNNOptions options)
    : detail::RNNImplBase<RNNImpl>(
          detail::RNNOptionsBase(options.input_size_, options.hidden_size_)
              .layers(options.layers_)
              .with_bias(options.with_bias_)
              .dropout(options.dropout_),
          /*cudnn_mode=*/static_cast<CuDNNMode>(options.activation_)),
      options_(options) {
  switch (options_.activation_) {
    case RNNActivation::ReLU: {
      activation_function_ = at::relu;
      break;
    }
    case RNNActivation::Tanh: {
      activation_function_ = at::tanh;
      break;
    }
  }
}

Tensor RNNImpl::cell_forward(Tensor input, Tensor state, int64_t layer) {
  auto hx = state.defined()
      ? state
      : torch::zeros({input.size(0), options_.hidden_size_}, input.options());

  auto h = linear(input, ihw_[layer], ihb_[layer]) +
      linear(hx, hhw_[layer], hhb_[layer]);

  return at::stack(activation_function_(h));
}

const RNNOptions& RNNImpl::options() const noexcept {
  return options_;
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ LSTM ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

LSTMImpl::LSTMImpl(LSTMOptions options)
    : detail::RNNImplBase<LSTMImpl>(
          options,
          /*cudnn_mode=*/CuDNNMode::LSTM,
          /*number_of_gates=*/4,
          /*has_cell_state=*/true) {}

Tensor LSTMImpl::cell_forward(Tensor input, Tensor state, int64_t layer) {
  auto hid = state.defined()
      ? state
      : torch::zeros(
            {2, input.size(0), options_.hidden_size_}, input.options());
  auto hx = hid[0];
  auto cx = hid[1];

  auto gates = linear(input, ihw_[layer], ihb_[layer]) +
      linear(hx, hhw_[layer], hhb_[layer]);

  auto chunked = gates.chunk(4, 1);
  auto in_gate = chunked[0].sigmoid();
  auto forget_gate = chunked[1].sigmoid();
  auto cell_gate = chunked[2].tanh();
  auto out_gate = chunked[3].sigmoid();

  auto cy = (forget_gate * cx) + (in_gate * cell_gate);
  auto hy = out_gate * cy.tanh();

  return at::stack(TensorListView{hy, cy}, 0);
}

const LSTMOptions& LSTMImpl::options() const noexcept {
  return options_;
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ GRU ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

GRUImpl::GRUImpl(GRUOptions options)
    : detail::RNNImplBase<GRUImpl>(
          options,
          /*cudnn_mode=*/CuDNNMode::GRU,
          /*number_of_gates=*/3) {}

Tensor GRUImpl::cell_forward(Tensor input, Tensor state, int64_t layer) {
  auto hx = state.defined()
      ? state
      : torch::zeros({input.size(0), options_.hidden_size_}, input.options());

  auto gi = linear(input, ihw_[layer], ihb_[layer]);
  auto gh = linear(input, hhw_[layer], hhb_[layer]);
  auto gic = gi.chunk(3, 1);
  auto ghc = gh.chunk(3, 1);

  auto reset_gate = (gic[0] + ghc[0]).sigmoid_();
  auto input_gate = (gic[1] + ghc[1]).sigmoid_();
  auto new_gate = (gic[2] + reset_gate * ghc[2]).tanh_();
  auto hy = new_gate + input_gate * (hx - new_gate);

  return at::stack(TensorListView(hy));
}

const GRUOptions& GRUImpl::options() const noexcept {
  return options_;
}
} // namespace nn
} // namespace torch
