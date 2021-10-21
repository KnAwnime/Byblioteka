#pragma once

#include <ATen/core/function_schema.h>
#include <ATen/core/ivalue.h>
#include <ATen/core/qualified_name.h>
#include <c10/util/Exception.h>
#include <c10/util/FunctionRef.h>

namespace c10 {
struct FunctionSchema;
};

namespace at {
TORCH_API void launch(std::function<void()> func);
}

namespace torch {
namespace jit {

struct Graph;
struct Code;

namespace mobile {
struct Code;
}

using Stack = std::vector<at::IValue>;
using Kwargs = std::unordered_map<std::string, at::IValue>;
struct RecursiveMethodCallError : public std::exception {};
using TaskLauncher = std::function<void(std::function<void()>)>;

TORCH_API void preoptimizeGraph(std::shared_ptr<Graph>& graph);

// A Function is a pure Graph with no implicit `self` object bound.
// It contains schema information and the executor that manages the
// execution of the function. Method is a wrapper around an
// underlying Function that also provides a `self` object.
struct TORCH_API Function {
  virtual c10::string_view doc_string() const {
    static constexpr c10::string_view no_doc_string = "";
    return no_doc_string;
  }

  virtual bool isGraphFunction() const {
    return false;
  }

  virtual void run(Stack& stack) = 0;

  virtual c10::intrusive_ptr<c10::ivalue::Future> runAsync(
      Stack& stack,
      TaskLauncher taskLauncher = at::launch) = 0;

  at::IValue operator()(
    Stack stack,
    const Kwargs& kwargs = Kwargs()) {
    getSchema().checkAndNormalizeInputs(stack, kwargs);
    run(stack);
    return stack.front();
  }

  virtual const c10::QualifiedName& qualname() const = 0;

  const std::string& name() const {
    return qualname().name();
  }

  // if this isn't yet defined, run its method_creator function
  virtual void ensure_defined() = 0;

  virtual const c10::FunctionSchema& getSchema() const = 0;

  virtual size_t num_inputs() const = 0;

  virtual Function& setSchema(c10::FunctionSchema schema) = 0;

  virtual bool call(Stack&, size_t, c10::function_ref<void(const Code&)>) {
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(false);
    return false;
  }

  virtual bool call(Stack&, c10::function_ref<void(const mobile::Code&)>) {
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(false);
    return false;
  }

  virtual ~Function() {}
};
} // namespace jit
} // namespace torch
