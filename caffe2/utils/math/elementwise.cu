#include "caffe2/utils/math/elementwise.h"

#include <type_traits>

#include <thrust/execution_policy.h>
#include <thrust/fill.h>
#include <thrust/functional.h>
#include <thrust/transform.h>

#include "caffe2/core/context_gpu.h"
#include "caffe2/utils/conversions.h"
#include "caffe2/utils/math/half_utils.h"
#include "caffe2/utils/math/utils.h"

namespace caffe2 {
namespace math {

namespace {

template <typename T>
__global__ void SinCosCUDAKernel(const int N, const T* X, T* S, T* C) {
  const int i = blockIdx.x * CAFFE_CUDA_NUM_THREADS + threadIdx.x;
  if (i < N) {
#if __CUDA_ARCH__ >= 350 || defined(__HIP_PLATFORM_HCC__)
    c10::cuda::compat::sincos(__ldg(X + i), S + i, C + i);
#else
    c10::cuda::compat::sincos(X[i], S + i, C + i);
#endif
  }
}

} // namespace

#define CAFFE2_SPECIALIZED_CUDA_SET(T)                                    \
  template <>                                                             \
  CAFFE2_CUDA_EXPORT void Set<T, CUDAContext>(                            \
      const int N, const T alpha, T* Y, CUDAContext* context) {           \
    if (N == 0) {                                                         \
      return;                                                             \
    }                                                                     \
    if (alpha == T(0)) {                                                  \
      cudaMemsetAsync(Y, 0, sizeof(T) * N, context->cuda_stream());       \
    } else {                                                              \
      thrust::fill(                                                       \
          thrust::cuda::par.on(context->cuda_stream()), Y, Y + N, alpha); \
    }                                                                     \
  }
CAFFE2_SPECIALIZED_CUDA_SET(bool)
CAFFE2_SPECIALIZED_CUDA_SET(char)
CAFFE2_SPECIALIZED_CUDA_SET(std::int8_t)
CAFFE2_SPECIALIZED_CUDA_SET(std::int16_t)
CAFFE2_SPECIALIZED_CUDA_SET(std::int32_t)
CAFFE2_SPECIALIZED_CUDA_SET(std::int64_t)
CAFFE2_SPECIALIZED_CUDA_SET(std::uint8_t)
CAFFE2_SPECIALIZED_CUDA_SET(std::uint16_t)
CAFFE2_SPECIALIZED_CUDA_SET(float)
CAFFE2_SPECIALIZED_CUDA_SET(double)
CAFFE2_SPECIALIZED_CUDA_SET(at::Half)
#undef CAFFE2_SPECIALIZED_CUDA_SET

#define DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(T, Func, DeviceFunc) \
  template <>                                                    \
  CAFFE2_CUDA_EXPORT void Func<T, CUDAContext>(                  \
      const int N, const T* X, T* Y, CUDAContext* context) {     \
    if (N > 0) {                                                 \
      thrust::transform(                                         \
          thrust::cuda::par.on(context->cuda_stream()),          \
          X,                                                     \
          X + N,                                                 \
          Y,                                                     \
          [] __device__(const T x) { return DeviceFunc(x); });   \
    }                                                            \
  }
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Exp, expf)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Log, logf)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Sin, sinf)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Asin, asinf)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Cos, cosf)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Acos, acosf)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Tan, tanf)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Atan, atanf)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Sinh, sinhf)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Cosh, coshf)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Tanh, tanhf)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Abs, fabsf)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Inv, utils::Inv<float>)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(double, Inv, utils::Inv<double>)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Sqr, utils::Square<float>)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Sqrt, sqrtf)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Rsqrt, rsqrtf)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(
    std::int32_t,
    Cube,
    utils::Cube<std::int32_t>)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(
    std::int64_t,
    Cube,
    utils::Cube<std::int64_t>)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Cube, utils::Cube<float>)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(double, Cube, utils::Cube<double>)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Cbrt, cbrtf)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Erf, erff)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(double, Erf, erf)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(bool, Not, utils::Not<bool>)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(
    std::int32_t,
    Neg,
    utils::Negate<std::int32_t>)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(
    std::int64_t,
    Neg,
    utils::Negate<std::int64_t>)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Neg, utils::Negate<float>)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(double, Neg, utils::Negate<double>)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(
    std::int32_t,
    Sign,
    utils::Sign<std::int32_t>)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(
    std::int64_t,
    Sign,
    utils::Sign<std::int64_t>)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(float, Sign, utils::Sign<float>)
DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION(double, Sign, utils::Sign<double>)
#undef DELEGATE_SIMPLE_CUDA_UNARY_FUNCTION

#define DELEGATE_CUDA_POWX(T, DeviceFunc)                               \
  template <>                                                           \
  CAFFE2_CUDA_EXPORT void Powx<T, CUDAContext>(                         \
      const int N, const T* A, const T b, T* Y, CUDAContext* context) { \
    thrust::transform(                                                  \
        thrust::cuda::par.on(context->cuda_stream()),                   \
        A,                                                              \
        A + N,                                                          \
        Y,                                                              \
        [b] __device__(const T x) { return DeviceFunc(x, b); });        \
  }
DELEGATE_CUDA_POWX(float, powf)
#undef DELEGATE_CUDA_POWX

#define CAFFE2_SPECIALIZED_CUDA_SINCOS(T)                             \
  template <>                                                         \
  CAFFE2_CUDA_EXPORT void SinCos<T, CUDAContext>(                     \
      const int N, const T* X, T* S, T* C, CUDAContext* context) {    \
    if (N > 0) {                                                      \
      const int K = DivUp(N, CAFFE_CUDA_NUM_THREADS);                 \
      SinCosCUDAKernel<T>                                             \
          <<<K, CAFFE_CUDA_NUM_THREADS, 0, context->cuda_stream()>>>( \
              N, X, S, C);                                            \
    }                                                                 \
  }
CAFFE2_SPECIALIZED_CUDA_SINCOS(float)
CAFFE2_SPECIALIZED_CUDA_SINCOS(double)
#undef CAFFE2_SPECIALIZED_CUDA_SINCOS

#define DELEGATE_CUDA_SCALE_BY_CUBLAS_FUNCTION(TAlpha, TData, CuBLASFunc)     \
  template <>                                                                 \
  CAFFE2_CUDA_EXPORT void Scale<TAlpha, TData, CUDAContext>(                  \
      const int N,                                                            \
      const TAlpha alpha,                                                     \
      const TData* X,                                                         \
      TData* Y,                                                               \
      CUDAContext* context) {                                                 \
    if (N == 0) {                                                             \
      return;                                                                 \
    }                                                                         \
    const TData alpha_host = static_cast<TData>(alpha);                       \
    if (Y == X) {                                                             \
      CUBLAS_ENFORCE(cublasSetPointerMode(                                    \
          context->cublas_handle(), CUBLAS_POINTER_MODE_HOST));               \
      CUBLAS_ENFORCE(                                                         \
          CuBLASFunc(context->cublas_handle(), N, &alpha_host, Y, 1));        \
    } else {                                                                  \
      thrust::transform(                                                      \
          thrust::cuda::par.on(context->cuda_stream()),                       \
          X,                                                                  \
          X + N,                                                              \
          Y,                                                                  \
          [alpha_host] __device__(const TData x) { return x * alpha_host; }); \
    }                                                                         \
  }                                                                           \
  template <>                                                                 \
  CAFFE2_CUDA_EXPORT void Scale<TAlpha, TData, CUDAContext>(                  \
      const int N,                                                            \
      const TAlpha* alpha,                                                    \
      const TData* X,                                                         \
      TData* Y,                                                               \
      CUDAContext* context) {                                                 \
    if (N == 0) {                                                             \
      return;                                                                 \
    }                                                                         \
    if (std::is_same<TAlpha, TData>::value && Y == X) {                       \
      CUBLAS_ENFORCE(cublasSetPointerMode(                                    \
          context->cublas_handle(), CUBLAS_POINTER_MODE_DEVICE));             \
      CUBLAS_ENFORCE(CuBLASFunc(                                              \
          context->cublas_handle(),                                           \
          N,                                                                  \
          reinterpret_cast<const TData*>(alpha),                              \
          Y,                                                                  \
          1));                                                                \
    } else {                                                                  \
      thrust::transform(                                                      \
          thrust::cuda::par.on(context->cuda_stream()),                       \
          X,                                                                  \
          X + N,                                                              \
          Y,                                                                  \
          [alpha] __device__(const TData x) {                                 \
            return x * static_cast<TData>(*alpha);                            \
          });                                                                 \
    }                                                                         \
  }
DELEGATE_CUDA_SCALE_BY_CUBLAS_FUNCTION(float, float, cublasSscal)
DELEGATE_CUDA_SCALE_BY_CUBLAS_FUNCTION(double, double, cublasDscal)
DELEGATE_CUDA_SCALE_BY_CUBLAS_FUNCTION(float, double, cublasDscal)
#undef DELEGATE_CUDA_SCALE_BY_CUBLAS_FUNCTION

#define CAFFE2_SPECIALIZED_CUDA_SCALE(TAlpha, TData)         \
  template <>                                                \
  CAFFE2_CUDA_EXPORT void Scale<TAlpha, TData, CUDAContext>( \
      const int N,                                           \
      const TAlpha alpha,                                    \
      const TData* X,                                        \
      TData* Y,                                              \
      CUDAContext* context) {                                \
    if (N > 0) {                                             \
      thrust::transform(                                     \
          thrust::cuda::par.on(context->cuda_stream()),      \
          X,                                                 \
          X + N,                                             \
          Y,                                                 \
          [alpha] __device__(const TData x) {                \
            return x * static_cast<TData>(alpha);            \
          });                                                \
    }                                                        \
  }                                                          \
  template <>                                                \
  CAFFE2_CUDA_EXPORT void Scale<TAlpha, TData, CUDAContext>( \
      const int N,                                           \
      const TAlpha* alpha,                                   \
      const TData* X,                                        \
      TData* Y,                                              \
      CUDAContext* context) {                                \
    if (N > 0) {                                             \
      thrust::transform(                                     \
          thrust::cuda::par.on(context->cuda_stream()),      \
          X,                                                 \
          X + N,                                             \
          Y,                                                 \
          [alpha] __device__(const TData x) {                \
            return x * static_cast<TData>(*alpha);           \
          });                                                \
    }                                                        \
  }
CAFFE2_SPECIALIZED_CUDA_SCALE(std::int32_t, std::int32_t)
CAFFE2_SPECIALIZED_CUDA_SCALE(std::int64_t, std::int64_t)
#undef CAFFE2_SPECIALIZED_CUDA_SCALE

#ifdef __HIP_PLATFORM_HCC__

#define CAFFE2_SPECIALIZED_CUDA_HALF_SCALE(TAlpha)                \
  template <>                                                     \
  CAFFE2_CUDA_EXPORT void Scale<TAlpha, at::Half, CUDAContext>(   \
      const int N,                                                \
      const TAlpha alpha,                                         \
      const at::Half* X,                                          \
      at::Half* Y,                                                \
      CUDAContext* context) {                                     \
    if (N > 0) {                                                  \
      const float alpha_host = convert::To<TAlpha, float>(alpha); \
      thrust::transform(                                          \
          thrust::cuda::par.on(context->cuda_stream()),           \
          X,                                                      \
          X + N,                                                  \
          Y,                                                      \
          [alpha_host] __device__(const at::Half x) {             \
            return convert::To<float, at::Half>(                  \
                convert::To<at::Half, float>(x) * alpha_host);    \
          });                                                     \
    }                                                             \
  }                                                               \
  template <>                                                     \
  CAFFE2_CUDA_EXPORT void Scale<TAlpha, at::Half, CUDAContext>(   \
      const int N,                                                \
      const TAlpha* alpha,                                        \
      const at::Half* X,                                          \
      at::Half* Y,                                                \
      CUDAContext* context) {                                     \
    if (N > 0) {                                                  \
      thrust::transform(                                          \
          thrust::cuda::par.on(context->cuda_stream()),           \
          X,                                                      \
          X + N,                                                  \
          Y,                                                      \
          [alpha] __device__(const at::Half x) {                  \
            return convert::To<float, at::Half>(                  \
                convert::To<at::Half, float>(x) *                 \
                convert::To<TAlpha, float>(*alpha));              \
          });                                                     \
    }                                                             \
  }
CAFFE2_SPECIALIZED_CUDA_HALF_SCALE(at::Half)
CAFFE2_SPECIALIZED_CUDA_HALF_SCALE(float)
#undef CAFFE2_SPECIALIZED_CUDA_HALF_SCALE

#else // __HIP_PLATFORM_HCC__

#define DELEGATE_CUDA_HALF_SCALE_BY_CUBLAS_FUNCTION(              \
    TAlpha, CuBLASFunc, kAlphaType, kExecutionType)               \
  template <>                                                     \
  CAFFE2_CUDA_EXPORT void Scale<TAlpha, at::Half, CUDAContext>(   \
      const int N,                                                \
      const TAlpha alpha,                                         \
      const at::Half* X,                                          \
      at::Half* Y,                                                \
      CUDAContext* context) {                                     \
    if (N == 0) {                                                 \
      return;                                                     \
    }                                                             \
    if (Y == X) {                                                 \
      CUBLAS_ENFORCE(cublasSetPointerMode(                        \
          context->cublas_handle(), CUBLAS_POINTER_MODE_HOST));   \
      CUBLAS_ENFORCE(cublasScalEx(                                \
          context->cublas_handle(),                               \
          N,                                                      \
          &alpha,                                                 \
          kAlphaType,                                             \
          Y,                                                      \
          CUDA_R_16F,                                             \
          1,                                                      \
          kExecutionType));                                       \
    } else {                                                      \
      const float alpha_host = convert::To<TAlpha, float>(alpha); \
      thrust::transform(                                          \
          thrust::cuda::par.on(context->cuda_stream()),           \
          X,                                                      \
          X + N,                                                  \
          Y,                                                      \
          [alpha_host] __device__(const at::Half x) {             \
            return convert::To<float, at::Half>(                  \
                convert::To<at::Half, float>(x) * alpha_host);    \
          });                                                     \
    }                                                             \
  }                                                               \
  template <>                                                     \
  CAFFE2_CUDA_EXPORT void Scale<TAlpha, at::Half, CUDAContext>(   \
      const int N,                                                \
      const TAlpha* alpha,                                        \
      const at::Half* X,                                          \
      at::Half* Y,                                                \
      CUDAContext* context) {                                     \
    if (N == 0) {                                                 \
      return;                                                     \
    }                                                             \
    if (Y == X) {                                                 \
      CUBLAS_ENFORCE(cublasSetPointerMode(                        \
          context->cublas_handle(), CUBLAS_POINTER_MODE_HOST));   \
      CUBLAS_ENFORCE(cublasScalEx(                                \
          context->cublas_handle(),                               \
          N,                                                      \
          alpha,                                                  \
          kAlphaType,                                             \
          Y,                                                      \
          CUDA_R_16F,                                             \
          1,                                                      \
          kExecutionType));                                       \
    } else {                                                      \
      thrust::transform(                                          \
          thrust::cuda::par.on(context->cuda_stream()),           \
          X,                                                      \
          X + N,                                                  \
          Y,                                                      \
          [alpha] __device__(const at::Half x) {                  \
            return convert::To<float, at::Half>(                  \
                convert::To<at::Half, float>(x) *                 \
                convert::To<TAlpha, float>(*alpha));              \
          });                                                     \
    }                                                             \
  }
DELEGATE_CUDA_HALF_SCALE_BY_CUBLAS_FUNCTION(
    at::Half,
    cublasScalEx,
    CUDA_R_16F,
    CUDA_R_32F)
DELEGATE_CUDA_HALF_SCALE_BY_CUBLAS_FUNCTION(
    float,
    cublasScalEx,
    CUDA_R_32F,
    CUDA_R_32F)
#undef DELEGATE_CUDA_HALF_SCALE_BY_CUBLAS_FUNCTION

#endif // __HIP_PLATFORM_HCC__

#define DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(T, Func, DeviceFunc)        \
  template <>                                                            \
  CAFFE2_CUDA_EXPORT void Func<T, CUDAContext>(                          \
      const int N, const T* A, const T* B, T* C, CUDAContext* context) { \
    if (N > 0) {                                                         \
      thrust::transform(                                                 \
          thrust::cuda::par.on(context->cuda_stream()),                  \
          A,                                                             \
          A + N,                                                         \
          B,                                                             \
          C,                                                             \
          DeviceFunc);                                                   \
    }                                                                    \
  }
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(
    std::int32_t,
    Add,
    thrust::plus<std::int32_t>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(
    std::int64_t,
    Add,
    thrust::plus<std::int64_t>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(float, Add, thrust::plus<float>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(double, Add, thrust::plus<double>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(at::Half, Add, utils::HalfAddFunctor())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(
    std::int32_t,
    Sub,
    thrust::minus<std::int32_t>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(
    std::int64_t,
    Sub,
    thrust::minus<std::int64_t>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(float, Sub, thrust::minus<float>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(double, Sub, thrust::minus<double>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(at::Half, Sub, utils::HalfSubFunctor())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(
    std::int32_t,
    Mul,
    thrust::multiplies<std::int32_t>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(
    std::int64_t,
    Mul,
    thrust::multiplies<std::int64_t>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(float, Mul, thrust::multiplies<float>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(double, Mul, thrust::multiplies<double>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(at::Half, Mul, utils::HalfMulFunctor())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(
    std::int32_t,
    Div,
    thrust::divides<std::int32_t>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(
    std::int64_t,
    Div,
    thrust::divides<std::int64_t>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(float, Div, thrust::divides<float>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(double, Div, thrust::divides<double>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(at::Half, Div, utils::HalfDivFunctor())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(float, Min, thrust::minimum<float>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(double, Min, thrust::minimum<double>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(float, Max, thrust::maximum<float>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(double, Max, thrust::maximum<double>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(bool, And, thrust::logical_and<bool>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(bool, Or, thrust::logical_or<bool>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(bool, Xor, thrust::bit_xor<bool>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(bool, BitwiseAnd, thrust::bit_and<bool>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(
    std::int32_t,
    BitwiseAnd,
    thrust::bit_and<std::int32_t>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(
    std::int64_t,
    BitwiseAnd,
    thrust::bit_and<std::int64_t>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(bool, BitwiseOr, thrust::bit_or<bool>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(
    std::int32_t,
    BitwiseOr,
    thrust::bit_or<std::int32_t>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(
    std::int64_t,
    BitwiseOr,
    thrust::bit_or<std::int64_t>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(bool, BitwiseXor, thrust::bit_xor<bool>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(
    std::int32_t,
    BitwiseXor,
    thrust::bit_xor<std::int32_t>())
DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION(
    std::int64_t,
    BitwiseXor,
    thrust::bit_xor<std::int64_t>())
#undef DELEGATE_SIMPLE_CUDA_BINARY_FUNCTION

#define DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(T, Func, DeviceComp)          \
  template <>                                                               \
  CAFFE2_CUDA_EXPORT void Func<T, CUDAContext>(                             \
      const int N, const T* A, const T* B, bool* C, CUDAContext* context) { \
    if (N > 0) {                                                            \
      thrust::transform(                                                    \
          thrust::cuda::par.on(context->cuda_stream()),                     \
          A,                                                                \
          A + N,                                                            \
          B,                                                                \
          C,                                                                \
          DeviceComp);                                                      \
    }                                                                       \
  }
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(bool, EQ, thrust::equal_to<bool>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(
    std::int32_t,
    EQ,
    thrust::equal_to<std::int32_t>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(
    std::int64_t,
    EQ,
    thrust::equal_to<std::int64_t>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(float, EQ, thrust::equal_to<float>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(double, EQ, thrust::equal_to<double>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(bool, NE, thrust::not_equal_to<bool>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(
    std::int32_t,
    NE,
    thrust::not_equal_to<std::int32_t>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(
    std::int64_t,
    NE,
    thrust::not_equal_to<std::int64_t>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(float, NE, thrust::not_equal_to<float>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(
    double,
    NE,
    thrust::not_equal_to<double>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(bool, LT, thrust::less<bool>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(
    std::int32_t,
    LT,
    thrust::less<std::int32_t>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(
    std::int64_t,
    LT,
    thrust::less<std::int64_t>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(float, LT, thrust::less<float>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(double, LT, thrust::less<double>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(bool, LE, thrust::less_equal<bool>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(
    std::int32_t,
    LE,
    thrust::less_equal<std::int32_t>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(
    std::int64_t,
    LE,
    thrust::less_equal<std::int64_t>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(float, LE, thrust::less_equal<float>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(double, LE, thrust::less_equal<double>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(bool, GT, thrust::greater<bool>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(
    std::int32_t,
    GT,
    thrust::greater<std::int32_t>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(
    std::int64_t,
    GT,
    thrust::greater<std::int64_t>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(float, GT, thrust::greater<float>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(double, GT, thrust::greater<double>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(bool, GE, thrust::greater_equal<bool>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(
    std::int32_t,
    GE,
    thrust::greater_equal<std::int32_t>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(
    std::int64_t,
    GE,
    thrust::greater_equal<std::int64_t>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(float, GE, thrust::greater_equal<float>())
DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION(
    double,
    GE,
    thrust::greater_equal<double>())
#undef DELEGATE_SIMPLE_CUDA_COMPARE_FUNCTION

} // namespace math
} // namespace caffe2
