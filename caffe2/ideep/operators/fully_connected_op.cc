#include <caffe2/ideep/ideep_utils.h>

namespace caffe2 {

USE_IDEEP_DEF_ALIASES();

class IDEEPFullyConnectedOp final : public IDEEPOperator {
 public:
  USE_IDEEP_DEF_ALIASES();
  USE_IDEEP_OPERATOR_FUNCTIONS();

  IDEEPFullyConnectedOp(const OperatorDef& operator_def, Workspace* ws)
      : IDEEPOperator(operator_def, ws),
        axis_(OperatorBase::GetSingleArgument<int32_t>("axis", 1)),
        axis_w_(OperatorBase::GetSingleArgument<int32_t>("axis_w", 1)) {}
  virtual ~IDEEPFullyConnectedOp() {}

  bool RunOnDevice() override {
    const auto& X = Input(INPUT);
    const auto& filter = Input(FILTER);
    auto* Y = Output(OUTPUT);

    itensor X_in = X;
    auto X_dims = CanonicalDims(X_in.get_dims(), axis_);
    if (X_in.get_dims() != X_dims) {
      X_in.reshape(X_dims);
    }

    itensor filter_in = filter;
    auto filter_dims = CanonicalDims(filter_in.get_dims(), axis_w_);
    if (filter_in.get_dims() != filter_dims) {
      filter_in.reshape(filter_dims);
    }

    if (InputSize() > BIAS) {
      ideep::inner_product_forward::compute(X_in, filter_in, Input(BIAS), *Y);
    } else {
      ideep::inner_product_forward::compute(X_in, filter_in, *Y);
    }

    return true;
  }

 private:
  size_t axis_{1};
  size_t axis_w_{1};

  INPUT_TAGS(INPUT, FILTER, BIAS);
  OUTPUT_TAGS(OUTPUT);
};

class IDEEPFullyConnectedGradientOp final : public IDEEPOperator {
 public:
  USE_IDEEP_DEF_ALIASES();
  USE_IDEEP_OPERATOR_FUNCTIONS();

  IDEEPFullyConnectedGradientOp(const OperatorDef& operator_def, Workspace* ws)
      : IDEEPOperator(operator_def, ws),
        axis_(OperatorBase::GetSingleArgument<int32_t>("axis", 1)),
        axis_w_(OperatorBase::GetSingleArgument<int32_t>("axis_w", 1)) {}
  virtual ~IDEEPFullyConnectedGradientOp() {}

  bool RunOnDevice() override {
    const auto& X = Input(INPUT);
    const auto& filter = Input(FILTER);
    const auto& dY = Input(OUTPUT_GRAD);
    auto* dfilter = Output(FILTER_GRAD);
    auto* dbias = Output(BIAS_GRAD);

    itensor X_in = X;
    auto X_dims = CanonicalDims(X_in.get_dims(), axis_);
    if (X_in.get_dims() != X_dims) {
      X_in.reshape(X_dims);
    }

    itensor filter_in = filter;
    auto filter_dims = CanonicalDims(filter_in.get_dims(), axis_w_);
    if (filter_in.get_dims() != filter_dims) {
      filter_in.reshape(filter_dims);
    }

    ideep::inner_product_backward_weights::compute(X_in, dY, *dfilter, *dbias);

    /**
     * In mkl-dnn,weight gradient shape is determined by X_in,
     * so we should ensure that weight gradient shape is consistent with weight shape.
     */
    if (dfilter->get_dims() != filter.get_dims()) {
      dfilter->reshape(filter.get_dims());
    }

    if (OutputSize() > INPUT_GRAD) {
      ideep::inner_product_backward_data::compute(
          dY, filter_in, X.get_dims(), *Output(INPUT_GRAD));
    }

    return true;
  }

 private:
  size_t axis_{1};
  size_t axis_w_{1};

  INPUT_TAGS(INPUT, FILTER, OUTPUT_GRAD);
  OUTPUT_TAGS(FILTER_GRAD, BIAS_GRAD, INPUT_GRAD);
};

REGISTER_IDEEP_OPERATOR(FC, IDEEPFullyConnectedOp);
REGISTER_IDEEP_OPERATOR(FCGradient, IDEEPFullyConnectedGradientOp);

} // namespace caffe2
