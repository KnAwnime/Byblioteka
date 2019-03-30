#pragma once

#include <ATen/core/function_schema.h>
#include <c10/util/LeftRight.h>
#include <c10/util/Metaprogramming.h>
#include <c10/util/flat_hash_map.h>
#include <ATen/core/ivalue.h>
#include <ATen/core/dispatch/KernelFunction.h>

#include <array>
#include <atomic>
#include <iostream>
#include <mutex>
#include <type_traits>
#include <sstream>
#include <unordered_map>
#include <functional>

namespace c10 {

/**
 * The type of a user-supplied function to initialize the kernel cache.
 * this is stored together with the KernelFunction in the DispatchTable
 * so we can create a new cache instance when a kernel is looked up
 * from the dispatch table.
 */
using KernelCacheCreatorFunction = std::function<std::unique_ptr<c10::KernelCache> ()>;
/**
 * The dispatch table stores a pointer to a kernel function and a pointer
 * to a function initializing a cache for the kernel. If the kernel wants
 * to use the cache, they supply the state initializer when the kernel
 * is registered. When a kernel is looked up from the dispatcher, a new
 * cache instance is created for it and each call to that kernel will get
 * this same cache instance.
 */
struct DispatchTableEntry final {
  /*not-nullable*/ KernelFunction* kernel_func;
  /*not-nullable*/ KernelCacheCreatorFunction cache_creator_func;
};

namespace detail {
/// Kernel implementations in a thread-safe hash table.
class ThreadsafeOperatorTable_ final {
 public:
  void emplace(TensorTypeId key, const DispatchTableEntry& value, const std::string& operator_name) {
    bool res = map_.write([&](ska::flat_hash_map<TensorTypeId, DispatchTableEntry>& map) -> bool {
      auto result = map.emplace(key, value);
      return result.second;
    });
    if (!res) {
      AT_ERROR("Tried to register multiple kernels with same dispatch key '",
      dispatch_key_to_string(key), "' for operator '", operator_name ,"'.");
    }
  }

  void erase(TensorTypeId key, const std::string& operator_name) {
    map_.write([&](ska::flat_hash_map<TensorTypeId, DispatchTableEntry>& map) {
      auto num_removed = map.erase(key);

      assert(num_removed <= 1); // This is not a multi-map
      if (num_removed == 0) {
        AT_ERROR("Tried to deregister a kernel with dispatch key '",
                 dispatch_key_to_string(key), "' for operator '", operator_name,
                 "' but that kernel isn't registered. Registered dispatch keys are: ",
                 list_all_dispatch_keys(map));
      }
    });
  }

  const DispatchTableEntry* lookup(TensorTypeId key, const string& operator_name) const {
    return map_.read([&](const ska::flat_hash_map<TensorTypeId, DispatchTableEntry>& map) -> const DispatchTableEntry* {
      auto found = map.find(key);
      if (found != map.end()) {
        return &found->second;
      } else {
        AT_ERROR("Didn't find kernel to dispatch to for operator '", operator_name,
                 "'. Tried to look up kernel for dispatch key '", dispatch_key_to_string(key),
                 "'. Registered dispatch keys are: ", list_all_dispatch_keys(map));
      }
    });
  }

  bool isEmpty() const {
    return map_.read([&](const ska::flat_hash_map<TensorTypeId, DispatchTableEntry>& map) -> bool {
      return map.size() == 0;
    });
  }

 private:
   static std::string list_all_dispatch_keys(const ska::flat_hash_map<TensorTypeId, DispatchTableEntry>& map) {
     if (map.size() == 0) {
       return "";
     }
     std::ostringstream str;
     str << dispatch_key_to_string(map.begin()->first);
     for (auto iter = ++map.begin(); iter != map.end(); ++iter) {
       str << ", " << dispatch_key_to_string(iter->first);
     }
     return str.str();
   }

   static std::string dispatch_key_to_string(TensorTypeId id) {
      // TODO Find better way to stringify tensor type ids without relying on backend
      std::string name = "";
      try {
        name = toString(tensorTypeIdToBackend(id));
      } catch (const std::exception&) {
        // This can fail if the tensor type id is not one of the preregistered backends.
        // However, dispatch_key_to_string is used to generate error reports, that
        // means an error already has happened when entering this function.
        // We don't want inner errors during generation of a report for an
        // outer error. Just report an empty name instead.
      }
      return name + "[" + toString(id) + "]";
   }

   LeftRight<ska::flat_hash_map<TensorTypeId, DispatchTableEntry>> map_;
};
} // namespace detail

/**
 * Per-operator dispatch table.
 *
 * Given an operator specified by a FunctionSchema, this class records a dispatch
 * table for various kernels provided for this operator.  For example, if we
 * consider the operator add(Tensor, Tensor), the dispatch table for this
 * operator may contain implementations for various dynamic tensor types, such
 * as CPUTensorId, CUDATensorId, etc.
 */
class DispatchTable final {
 public:
  explicit DispatchTable(const FunctionSchema& schema)
  : kernels_()
  , dispatch_strategy_(get_dispatch_strategy_(schema))
  , operator_name_(schema.name()) {}

  DispatchTable(DispatchTable&&) = delete;
  DispatchTable& operator=(DispatchTable&&) = delete;
  DispatchTable(const DispatchTable&) = delete;
  DispatchTable& operator=(const DispatchTable&) = delete;

  /**
   * Register a kernel in the table at some dispatch key.
   * @param func Concrete kernel function implementation to register
   * @param dispatch_key Dispatch key to define when this kernel is selected
   */
  void registerKernel(
      TensorTypeId dispatch_key,
      const DispatchTableEntry& kernel) {
    kernels_.emplace(dispatch_key, kernel, operator_name_);
  }

  /**
   * Deregister the kernel for some dispatch key.
   *
   * @param dispatch_key Dispatch key to unregister.
   */
  // TODO: This isn't going to work so well when we get more complicated
  // override patterns! In this case, an operator will show up in multiple
  // slots, and erasing them one-by-one is probably not such a good idea.
  void deregisterKernel(TensorTypeId dispatch_key) {
    kernels_.erase(dispatch_key, operator_name_);
  }

  /**
   * Perform a dynamic dispatch on this table and find the kernel to call
   * for the given arguments.
   *
   * @param args Arguments to invoke the function with
   * @return Kernel function pointing to the right kernel for the given arguments
   */
   const DispatchTableEntry& lookup(const Stack* stack) const {
     TensorTypeId dispatch_key = dispatch_strategy_.get_dispatch_key(stack);
     return *kernels_.lookup(dispatch_key, operator_name_);
   }

   bool isEmpty() const {
     return kernels_.isEmpty();
   }

private:
  struct DispatchStrategy final {
    // this is caching the index so we don't have to parse the schema inputs
    // again and again for each dispatcher lookup.
    // reverse_index means this is the distance from the first tensor argument
    // to argument_list.end(), i.e. from the top of the stack.
    // Since it is distance to end(), this means it's 1-indexed,
    // i.e. '1' is the last argument.
    size_t reverse_index_of_first_tensor_arg_;
    bool first_tensor_arg_is_tensor_list_;

    TensorTypeId get_dispatch_key(const Stack* stack) const {
      auto first_tensor_arg = torch::jit::peek(
        *stack,
        0,
        reverse_index_of_first_tensor_arg_
      );
      if (first_tensor_arg_is_tensor_list_) {
        const auto& tensor_list = first_tensor_arg.toTensorListRef();
        if (tensor_list.size() == 0) {
          throw std::runtime_error("Tried to dispatch based on an empty tensor list. When the first tensor argument of an operator is a tensor list, then it must not be empty.");
        }
        return tensor_list[0].type_id();
      } else {
        // TODO Avoid bumping the refcounter
        return first_tensor_arg.toTensor().type_id();
      }
    }
  };

  static DispatchStrategy get_dispatch_strategy_(const FunctionSchema& schema) {
    for (size_t i = 0; i < schema.arguments().size(); ++i) {
      const auto& type = schema.arguments()[i].type();
      if (type->isSubtypeOf(TensorType::get())) {
        return {schema.arguments().size() - i, false};
      }
      if (type->isSubtypeOf(ListType::ofTensors())) {
        return {schema.arguments().size() - i, true};
      }
    }

    AT_ERROR("Tried to create dispatch table for operator schema ", schema.name(), " that doesn't have tensor arguments.");
  }

  detail::ThreadsafeOperatorTable_ kernels_;
  DispatchStrategy dispatch_strategy_;
  std::string operator_name_;
};

} // namespace c10
