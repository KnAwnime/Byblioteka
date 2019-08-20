#pragma once

#include <c10/core/Device.h>
#include <c10/core/Layout.h>
#include <c10/core/MemoryFormat.h>
#include <c10/core/QScheme.h>
#include <c10/core/Scalar.h>
#include <c10/core/ScalarType.h>
#include <c10/core/Storage.h>
#include <ATen/core/TensorAccessor.h>
#include <c10/core/TensorImpl.h>
#include <c10/core/UndefinedTensorImpl.h>
#include <c10/util/Exception.h>
#include <c10/util/Optional.h>
#include <c10/util/intrusive_ptr.h>
#include <ATen/core/LegacyTypeDispatch.h>
#include <ATen/core/DeprecatedTypePropertiesRegistry.h>
#ifdef BUILD_NAMEDTENSOR
#include <ATen/NamedTensor.h>
#endif

namespace caffe2 {
class Tensor;
}
namespace c10{
struct TensorOptions;
}
namespace at {
struct Generator;
struct Type;
class DeprecatedTypeProperties;
class Tensor;
} // namespace at

namespace at {

class Tensor;
using TensorList = ArrayRef<Tensor>;

struct Quantizer;
// This is temporary typedef to enable Quantizer in aten native function API
// we'll remove them when we are actually exposing Quantizer class
// to frontend
using ConstQuantizerPtr = const c10::intrusive_ptr<Quantizer>&;

// Tensor is a "generic" object holding a pointer to the underlying TensorImpl object, which
// has an embedded reference count. In this way, Tensor is similar to boost::intrusive_ptr.
//
// For example:
//
// void func(Tensor a) {
//   Tensor b = a;
//   ...
// }
//
// In this example, when we say Tensor b = a, we are creating a new object that points to the
// same underlying TensorImpl, and bumps its reference count. When b goes out of scope, the
// destructor decrements the reference count by calling release() on the TensorImpl it points to.
// The existing constructors, operator overloads, etc. take care to implement the correct semantics.
//
// Note that Tensor can also be NULL, i.e. it is not associated with any underlying TensorImpl, and
// special care must be taken to handle this.
class CAFFE2_API Tensor {
 public:
  Tensor(){};
  // This constructor should not be used by end users and is an implementation
  // detail invoked by autogenerated code.
  explicit Tensor(
      c10::intrusive_ptr<TensorImpl, UndefinedTensorImpl> tensor_impl)
      : impl_(std::move(tensor_impl)) {
    if (impl_.get() == nullptr) {
      throw std::runtime_error("TensorImpl with nullptr is not supported");
    }
  }
  Tensor(const Tensor&) = default;
  Tensor(Tensor&&) = default;


 public:
  // Creates a new wrapper from TensorImpl. Intentionally a free method because
  // it should be used with care. Checks necessary invariants
  static Tensor wrap_tensor_impl(
      c10::intrusive_ptr<TensorImpl, UndefinedTensorImpl> tensor_impl) {
    Tensor r(std::move(tensor_impl));
    r.enforce_invariants();
    return r;
  }

  int64_t dim() const {
    return impl_->dim();
  }
  int64_t storage_offset() const {
    return impl_->storage_offset();
  }

  TensorImpl * unsafeGetTensorImpl() const {
    return impl_.get();
  }
  TensorImpl * unsafeReleaseTensorImpl() {
    return impl_.release();
  }
  const c10::intrusive_ptr<TensorImpl, UndefinedTensorImpl>& getIntrusivePtr() const {
    return impl_;
  }

  bool defined() const {
    return impl_;
  }

  void reset() {
    impl_.reset();
  }

  // The following overloads are very intruiging.  Consider the following
  // program:
  //
  //    x[1] = 3;
  //
  // We would expect that the first entry of x is written to 3.  But how can we
  // actually achieve this?  x[1] evaluates to a tensor...
  //
  // The answer is, using a ref-qualifier.  x[1] is an rvalue, which cannot be
  // (profitably) assigned to in the traditional sense, so we overload
  // assignment to mean, "Actually, copy 3 into the tensor data."  This is done
  // with an rvalue-reference ref-qualified overload (the methods with && at the
  // end of their type.)
  //
  // There's one more fly in the ointment: We also want
  //
  //    Tensor x = y;
  //
  // to work, and we want it NOT to copy.  So we need a traditional operator=
  // overload.  But we MUST specify a mutable lvalue ref-qualifier, to
  // disambiguate the traditional overload from the rvalue-reference
  // ref-qualified overload.  Otherwise, it will be ambiguous, because
  // a non ref-qualified method is eligible for all situations.

  // Unfortunately, we have to write these constructors out manually
  // to work around an MSVC bug:
  //    error C2580: 'at::Tensor &at::Tensor::operator =(const at::Tensor &) &':
  //    multiple versions of a defaulted special member functions are not allowed
  // Tensor& operator=(const Tensor&) & = default;
  // Tensor& operator=(Tensor&&) & = default;
  Tensor& operator=(const Tensor& x) & {
    impl_ = x.impl_;
    return *this;
  }
  Tensor& operator=(Tensor&& x) & {
    impl_ = std::move(x.impl_);
    return *this;
  }

  Tensor& operator=(Scalar v) &&;
  Tensor& operator=(const Tensor&) &&;
  Tensor& operator=(Tensor&&) &&;

  bool is_same(const Tensor& other) const noexcept {
    return impl_ == other.impl_;
  }
  size_t use_count() const noexcept {
    return impl_.use_count();
  }
  size_t weak_use_count() const noexcept {
    return impl_.weak_use_count();
  }

  std::string toString() const;

  IntArrayRef sizes() const {
    return impl_->sizes();
  }
  IntArrayRef strides() const {
    return impl_->strides();
  }
#ifdef BUILD_NAMEDTENSOR
  optional<DimnameList> names() const {
    return impl::get_names(unsafeGetTensorImpl());
  }
#endif
  int64_t ndimension() const {
    return dim();
  }
  bool is_contiguous(at::MemoryFormat memory_format=at::MemoryFormat::Contiguous) const {
    return impl_->is_contiguous(memory_format);
  }

  at::MemoryFormat suggest_memory_format() const {
    if (impl_->is_strides_like_channels_last()) {
      return at::MemoryFormat::ChannelsLast;
    }
    return at::MemoryFormat::Contiguous;
  }

  // Total bytes consumed by the "view" of elements of the array.  Does not
  // include size of metadata.  The number reported here does not necessarily
  // correspond to the true physical memory consumed by a tensor; instead,
  // it reports the memory the tensor would take *if* it were contiguous.
  // Defined to be numel() * itemsize()
  size_t nbytes() const {
    return impl_->numel() * impl_->itemsize();
  }

  // Length of one array element in bytes.  This is the traditional
  // Numpy naming.
  size_t itemsize() const {
    return impl_->itemsize();
  }

  // Same as itemsize().  This is the PyTorch naming.
  size_t element_size() const {
    return impl_->itemsize();
  }

  DeprecatedTypeProperties & type() const {
    return globalDeprecatedTypePropertiesRegistry().getDeprecatedTypeProperties(
        tensorTypeIdToBackend(type_id()),
        scalar_type(),
        is_variable());
  }
  TensorTypeId type_id() const {
    return impl_->type_id();
  }
  ScalarType scalar_type() const {
    return typeMetaToScalarType(impl_->dtype());
  }
  bool has_storage() const {
    return defined() && impl_->has_storage();
  }
  const Storage& storage() const {
    return impl_->storage();
  }
  bool is_alias_of(const at::Tensor& other) const{
    return impl_->storage().is_alias_of(other.storage());
  }
  Tensor toType(const DeprecatedTypeProperties & t, bool non_blocking=false) const;
  Tensor toType(ScalarType t) const;
  Tensor toBackend(Backend b) const;

  /// Returns true if the `Tensor` is actually a `torch::autograd::Variable`.
  /// Defined in Type.h because of include order issues.
  bool is_variable() const noexcept;

  /// Returns a `Tensor`'s layout. Defined in Type.h
  Layout layout() const noexcept;

  /// Returns a `Tensor`'s dtype (`TypeMeta`). Defined in TensorMethods.h
  caffe2::TypeMeta dtype() const noexcept;

  /// Returns a `Tensor`'s device.
  Device device() const;

  /// Returns a `Tensor`'s device index.
  int64_t get_device() const;

  /// Returns if a `Tensor` has CUDA backend.
  bool is_cuda() const;

  /// Returns if a `Tensor` has HIP backend.
  bool is_hip() const;

  /// Returns if a `Tensor` has sparse backend.
  bool is_sparse() const;

  /// Returns if a `Tensor` is mkldnn tensor.
  bool is_mkldnn() const;

  /// Returns if a `Tensor` has quantized backend.
  bool is_quantized() const;

#ifdef BUILD_NAMEDTENSOR
  /// Returns if a `Tensor` has any dimension names
  bool has_names() const;

  /// Returns a `Tensor`'s dimension names data structure
  const NamedTensorMeta* get_named_tensor_meta() const;
  NamedTensorMeta* get_named_tensor_meta();
#endif

  /// Returns the `TensorOptions` corresponding to this `Tensor`. Defined in
  /// TensorOptions.h.
  TensorOptions options() const;

  void* data_ptr() const {
    return this->unsafeGetTensorImpl()->data();
  }

  template <typename T>
  T * data_ptr() const;

  template<typename T>
  T * data() const {
    return data_ptr<T>();
  }

  template <typename T>
  T item() const;

  // Purposely not defined here to avoid inlining
  void print() const;

  // Return a `TensorAccessor` for CPU `Tensor`s. You have to specify scalar type and
  // dimension.
  template<typename T, size_t N>
  TensorAccessor<T,N> accessor() const& {
    static_assert(N > 0, "accessor is used for indexing tensor, for scalars use *data<T>()");
    TORCH_CHECK(dim() == N, "expected ", N, " dims but tensor has ", dim());
    return TensorAccessor<T,N>(data<T>(),sizes().data(),strides().data());
  }
  template<typename T, size_t N>
  TensorAccessor<T,N> accessor() && = delete;

  // Return a `PackedTensorAccessor` for CUDA `Tensor`s. You have to specify scalar type and
  // dimension. You can optionally specify RestrictPtrTraits as a template parameter to
  // cast the data pointer to a __restrict__ pointer.
  // In order to use this, your CUDA kernel has to take a corresponding PackedTensorAccessor
  // as an argument.
  template<typename T, size_t N, template <typename U> class PtrTraits = DefaultPtrTraits, typename index_t = int64_t>
  PackedTensorAccessor<T,N,PtrTraits,index_t> packed_accessor() const& {
    static_assert(N > 0, "accessor is used for indexing tensor, for scalars use *data<T>()");
    TORCH_CHECK(dim() == N, "expected ", N, " dims but tensor has ", dim());
    return PackedTensorAccessor<T,N,PtrTraits,index_t>(static_cast<typename PtrTraits<T>::PtrType>(data<T>()),sizes().data(),strides().data());
  }
  template<typename T, size_t N,  template <typename U> class PtrTraits = DefaultPtrTraits, typename index_t = int64_t>
  PackedTensorAccessor<T,N> packed_accessor() && = delete;

  Tensor operator-() const;
  Tensor& operator+=(const Tensor & other);
  Tensor& operator+=(Scalar other);
  Tensor& operator-=(const Tensor & other);
  Tensor& operator-=(Scalar other);
  Tensor& operator*=(const Tensor & other);
  Tensor& operator*=(Scalar other);
  Tensor& operator/=(const Tensor & other);
  Tensor& operator/=(Scalar other);
  Tensor operator[](Scalar index) const;
  Tensor operator[](Tensor index) const;
  Tensor operator[](int64_t index) const;

  Tensor cpu() const;
  Tensor cuda() const;
  Tensor hip() const;

  // ~~~~~ Autograd API ~~~~~

  Tensor& set_requires_grad(bool requires_grad) {
    impl_->set_requires_grad(requires_grad);
    return *this;
  }
  bool requires_grad() const {
    return impl_->requires_grad();
  }

  Tensor& grad() {
    return impl_->grad();
  }
  const Tensor& grad() const {
    return impl_->grad();
  }

  // STOP.  Thinking of adding a method here, which only makes use
  // of other ATen methods?  Define it in native_functions.yaml.

  //example
  //Tensor * add(Tensor & b);
  ${tensor_method_declarations}

  // We changed .dtype() to return a TypeMeta in #12766. Ideally, we want the
  // at::kDouble and its friends to be TypeMeta's, but that hasn't happened yet.
  // Before that change, we make this method to maintain BC for C++ usage like
  // `x.to(y.dtype)`.
  // TODO: remove following two after at::kDouble and its friends are TypeMeta's.
  inline Tensor to(caffe2::TypeMeta type_meta, bool non_blocking=false, bool copy=false) const {
    return this->to(/*scalar_type=*/typeMetaToScalarType(type_meta), non_blocking, copy);
  }
  inline Tensor to(Device device, caffe2::TypeMeta type_meta, bool non_blocking=false, bool copy=false) const {
    return this->to(device, /*scalar_type=*/typeMetaToScalarType(type_meta), non_blocking, copy);
  }

  template <typename F, typename... Args>
  auto m(F func, Args&&... params) const -> decltype(func(*this, std::forward<Args>(params)...)) {
    return func(*this, std::forward<Args>(params)...);
  }

protected:
  friend class ::caffe2::Tensor;

  void enforce_invariants();
  c10::intrusive_ptr<TensorImpl, UndefinedTensorImpl> impl_;
};

namespace detail {
// Helper creator for Tensor clas which doesn't requires the users to pass
// in an intrusive_ptr instead it just converts the argument passed to
// requested intrusive_ptr type.
template <typename T, typename... Args>
Tensor make_tensor(Args&&... args) {
  return Tensor(c10::make_intrusive<T>(std::forward<Args>(args)...));
}

inline Backend infer_backend(const Tensor & t) {
  TORCH_CHECK(t.defined(), "undefined Tensor");
  return tensorTypeIdToBackend(t.type_id());
}
inline Backend infer_backend(const TensorList & tl) {
  TORCH_CHECK(tl.size() > 0, "expected a non-empty list of Tensors");
  return tensorTypeIdToBackend(tl[0].type_id());
}

inline bool infer_is_variable(const Tensor & t) {
  TORCH_CHECK(t.defined(), "undefined Tensor");
  return t.is_variable();
}
inline bool infer_is_variable(const TensorList & tl) {
  TORCH_CHECK(tl.size() > 0, "expected a non-empty list of Tensors");
  return tl[0].is_variable();
}
} // namespace detail

} // namespace at

#include <ATen/core/TensorMethods.h>
