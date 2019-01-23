#include <torch/csrc/jit/tracer.h>

#include <torch/csrc/autograd/engine.h>
#include <torch/csrc/autograd/function.h>
#include <torch/csrc/autograd/variable.h>
#include <c10/util/Exception.h>
#include <torch/csrc/jit/passes/dead_code_elimination.h>
#include <torch/csrc/jit/passes/remove_expands.h>

#include <memory>
#include <sstream>
#include <string>

namespace torch {
namespace jit {
namespace tracer {

////////////////////////////////////////////////////////////////////////////////
// Recording the traces
////////////////////////////////////////////////////////////////////////////////
namespace detail {

template <typename T>
void genericAddInput(Node* n, T value) {
  Value* v = n->owningGraph()->insertConstant(value);
  recordSourceLocation(v->node());
  n->addInput(v);
}

template <typename T>
void badArgType(const T& v) {
  AT_ERROR(
      "Found an unsupported argument type in the JIT tracer: ",
      c10::demangle_type<T>(),
      ". File a bug report.");
}

thread_local std::shared_ptr<TracingState> tracing_state;

} // namespace detail

// Given a variable 'var', return the 'node' which represents the instruction
// which computes the value of this variable in the IR.
// Here, we interpret untraced variables as constants that are just embedded
// in the graph.  This is useful to handle code which does things like this
// (from torch.autograd.variable, now moved to C++):
//
//    def mm(self, matrix):
//      output = Variable(self.data.new(self.data.size(0), matrix.data.size(1)))
//      return Addmm.apply(output, self, matrix, 0, 1, True)
//
// Here, mm fakes up a dummy variable with uninitialized data to do an inplace
// update on, but subsequently ignores it because the alpha scaling factor is
// zero. This is one of the cases where a Variable can be created inside of a
// trace, and if we treat it as a constant, everything will work out.
Value* getValueTrace(const Variable& var) {
  auto& state = getTracingState();
  if (!var.defined()) {
    Node* n = state->graph->createUndefined();
    return state->graph->insertNode(n)->output();
  }

  auto& env_stack = getTracingState()->env_stack;

  for (size_t i = 0; i < env_stack.size(); ++i) {
    auto& value_map = env_stack.at(env_stack.size() - 1 - i).value_map;
    auto it = value_map.find(var);
    if (it == value_map.end())
      continue;
    if (!it->second->hasUniqueName()) {
      auto unique_name = getTracingState()->lookup_var_name_fn(var);
      if (!unique_name.empty()) {
        it->second->setUniqueName(unique_name);
      }
    }
    return it->second;
  }

  // Didn't find it. Bake in a constant
  Value* constant = state->graph->insertConstant(var.data());
  recordSourceLocation(constant->node());
  constant->inferTypeFrom(var.data());
  auto it = env_stack.back().value_map.find(var);
  it = env_stack.back().value_map.emplace_hint(it, var, constant);
  return it->second;
}

void setValueTrace(const IValue& v, Value* value) {
  if (v.isTensor()) {
    auto var = v.toTensor();
    JIT_ASSERT(var.defined());
    getTracingState()->env_stack.back().value_map[var] = value;
  } else if (v.isTensorList()) {
    auto& outputs = v.toTensorList()->elements();
    auto graph = getTracingState()->graph;
    Node* unpack_node = graph->insertNode(
      graph->createListUnpack(value, outputs.size()));
    for (size_t i = 0; i < outputs.size(); ++i) {
      setValueTrace(outputs[i], unpack_node->outputs()[i]);
    }
  } else if (v.isTuple()) {
    auto& outputs = v.toTuple()->elements();
    auto graph = getTracingState()->graph;
    Node* unpack_node = graph->insertNode(
      graph->createTupleUnpack(value));
    for (size_t i = 0; i < outputs.size(); ++i) {
      setValueTrace(outputs[i], unpack_node->outputs()[i]);
    }
  } else if (v.isGenericList()) {
    auto elements = v.toGenericListRef();
    auto graph = getTracingState()->graph;
    Node* unpack_node = graph->insertNode(
      graph->createListUnpack(value, elements.size()));
    for (size_t i = 0; i < elements.size(); ++i) {
      setValueTrace(elements[i], unpack_node->outputs()[i]);
    }
  } else {
    std::ostringstream os;
    os << "Tracer cannot set value trace for type " << v.tagKind() << ". "
       << "Supported types are tensor, tensor list, and tuple of tensors.";
    throw std::runtime_error(os.str());
  }
}

void setFutureTrace(const c10::intrusive_ptr<c10::ivalue::Future>& fut, Value* value) {
  getTracingState()->env_stack.back().future_map[fut] = value;
}

Value* getFutureTrace(const c10::intrusive_ptr<c10::ivalue::Future>& fut) {
  auto& env_stack = getTracingState()->env_stack;

  for (size_t i = 0; i < env_stack.size(); ++i) {
    auto& future_map = env_stack.at(env_stack.size() - 1 - i).future_map;
    auto it = future_map.find(fut);
    if (it == future_map.end())
      continue;
    return it->second;
  }

    std::ostringstream oss;
    oss << "Tried to trace Future that the tracer was not aware of.";
    throw std::runtime_error(oss.str());
}


void addInputs(Node* n, const char* name, int64_t value) {
  using ArgumentStash = jit::tracer::ArgumentStash;
  if (ArgumentStash::hasValue(name)) {
    Value* v = ArgumentStash::popValue(name);
    n->addInput(v);
  } else {
    detail::genericAddInput(n, value);
  }
}

void addInputs(Node* n, const char* name, c10::optional<int64_t> value) {
  if (value) {
    detail::genericAddInput(n, *value);
  } else {
    Graph* g = n->owningGraph();
    Value* none = g->insertNode(g->createNone(IntType::get()))->output();
    n->addInput(none);
  }
}
void addInputs(Node* n, const char* name, bool value) {
  detail::genericAddInput(n, value);
}
void addInputs(Node* n, const char* name, double value) {
  detail::genericAddInput(n, value);
}
void addInputs(Node* n, const char* name, const at::Scalar& value) {
  detail::genericAddInput(n, value);
}
void addInputs(
    Node* n,
    const char* name,
    const c10::optional<at::Scalar>& value) {
  if (value) {
    detail::genericAddInput(n, *value);
  } else {
    Graph* g = n->owningGraph();
    Value* none = g->insertNode(g->createNone(NumberType::get()))->output();
    n->addInput(none);
  }
}
void addInputs(Node* n, const char* name, const std::string& value) {
  detail::genericAddInput(n, value);
}
void addInputs(Node* n, const char* name, const at::Tensor& value) {
  n->addInput(getValueTrace(value));
}
void addInputs(Node* n, const char* name, const at::SparseTensorRef& value) {
  detail::badArgType(value);
}
void addInputs(Node* n, const char* name, at::Generator* value) {
  if (value) {
    detail::badArgType(value);
  }
  Graph* g = n->owningGraph();
  Value* undef_gen =
      g->insertNode(g->createNone(GeneratorType::get()))->output();
  n->addInput(undef_gen);
}
void addInputs(Node* n, const char* name, at::Device value) {
  detail::genericAddInput(n, value);
}
void addInputs(Node* n, const char* name, at::Layout value) {
  detail::genericAddInput(n, static_cast<int64_t>(value));
}
void addInputs(Node* n, const char* name, at::ScalarType value) {
  detail::genericAddInput(n, static_cast<int64_t>(value));
}
void addInputs(
    Node* n,
    const char* name,
    const c10::optional<at::ScalarType>& value) {
  if (value) {
    detail::genericAddInput(n, static_cast<int64_t>(*value));
  } else {
    Graph* g = n->owningGraph();
    Value* none = g->insertNode(g->createNone(IntType::get()))->output();
    n->addInput(none);
  }
}

void addInputs(Node* n, const char* name, at::TensorList value) {
  Graph* g = n->owningGraph();
  Node* list_node = g->insertNode(
      g->createList(DynamicType::get(), fmap(value, getValueTrace)));
  n->addInput(list_node->output());
}

void addInputs(Node* n, const char* name, const at::TensorOptions& options) {
  // [TensorOptions in script] - update this when you change how we schematize
  // TensorOptions
  addInputs(n, name, at::typeMetaToScalarType(options.dtype()));
  addInputs(n, name, options.layout());
  addInputs(n, name, options.device());
}

void addInputs(Node* n, const char* name, at::IntList value) {
  using ArgumentStash = jit::tracer::ArgumentStash;
  std::vector<Value*> info = ArgumentStash::hasIntList(name)
      ? ArgumentStash::popIntList(name)
      : ArgumentStash::IntListTrace(value.size());

  auto& g = getTracingState()->graph;
  for (size_t i = 0; i < info.size(); ++i) {
    if (info[i] != nullptr)
      continue;
    info[i] = g->insertConstant(value[i]);
    recordSourceLocation(info[i]->node());
  }
  for (jit::Value* v : info) {
    if (*v->type() != *jit::IntType::get()) {
      throw std::runtime_error(
          "Type mismatch in setposattr for IntList. Check that your program "
          "is valid without tracing, and please file a bug report if it is.");
    }
  }
  n->addInput(
      g->insertNode(g->createList(jit::IntType::get(), info))->output());
}

void addInputs(Node* n, const char* name, const ArrayRef<double>& value) {
  AT_ERROR("Tracing float lists currently not supported!");
}

void addOutput(Node* node, const at::Tensor& output) {
  setOutput(node->addOutput(), output);
}

void setOutput(Value* value, const at::Tensor& output) {
  if (output.defined()) {
    value->inferTypeFrom(output);
    setValueTrace(autograd::as_variable_ref(output), value);
  }
}

void addOutput(Node* node, const std::vector<at::Tensor>& outputs) {
  Value* value = node->addOutput()->setType(ListType::ofTensors());
  Graph* graph = node->owningGraph();
  Node* unpack_node = graph->insertNode(
      graph->create(prim::ListUnpack, {value}, outputs.size()));
  for (size_t i = 0; i < outputs.size(); ++i) {
    Value* output_val = unpack_node->outputs()[i];
    output_val->inferTypeFrom(outputs[i]);
    setValueTrace(outputs[i], output_val);
  }
}

const std::shared_ptr<TracingState>& getTracingState() {
  return detail::tracing_state;
}

void setTracingState(std::shared_ptr<TracingState> state) {
  detail::tracing_state = std::move(state);
}

TracingState::TracingState() : env_stack{TracingEnvironmentFrame()}, graph(new Graph()) {}

TracingState::~TracingState() = default;

autograd::Variable getSizeOf(const autograd::Variable& var, int64_t dim) {
  auto& tracing_state = getTracingState();
  auto& graph = tracing_state->graph;

  auto size_var =
      autograd::make_variable(scalar_to_tensor(at::Scalar(var.size(dim))));
  auto* value = getValueTrace(var);
  auto dim_val = graph->insertConstant(dim);
  recordSourceLocation(dim_val->node());
  auto* node = graph->insertNode(graph->create(aten::size, {value, dim_val}));
  recordSourceLocation(node);
  node->output()->setType(jit::IntType::get());

  auto ten =
      graph->insertNode(graph->createNumToTensor(node->output()))->output();
  setValueTrace(size_var, ten);
  return size_var;
}

////////////////////////////////////////////////////////////////////////////////
// Argument stash
////////////////////////////////////////////////////////////////////////////////
thread_local ArgumentStash ArgumentStash::stash;

void ArgumentStash::stashIntListElem(
    const std::string& arg_name,
    size_t size,
    size_t idx,
    const Variable& var) {
  // TODO: check type?
  if (!isTracing())
    return;
  auto& list_trace = stash.intlists.emplace(arg_name, size).first->second;
  AT_ASSERT(size == list_trace.size());
  AT_ASSERT(idx < list_trace.size());
  AT_ASSERT(list_trace[idx] == nullptr);

  Value* ten = getValueTrace(var);
  auto& g = *ten->owningGraph();
  WithInsertPoint guard(ten->node()->next());
  auto prim = g.insert(prim::Int, {ten});
  list_trace[idx] = prim;
}

void ArgumentStash::stashValue(
    const std::string& arg_name,
    size_t idx,
    const Variable& var,
    const TypePtr& type) {
  if (!isTracing())
    return;

  Value* ten = getValueTrace(var);
  WithInsertPoint guard(ten->node()->next());
  auto& g = *ten->owningGraph();

  if (type == IntType::get()) {
    ten = g.insert(prim::Int, {ten});
  } else if (type == FloatType::get()) {
    ten = g.insert(prim::Float, {ten});
  }

  stash.values.emplace(arg_name, ten);
}

////////////////////////////////////////////////////////////////////////////////
// Stack trace recording
////////////////////////////////////////////////////////////////////////////////
// no python present so we just do not record source information
void defaultRecordSourceLocation(Node* n) {}
std::atomic<decltype(&defaultRecordSourceLocation)> record_source_location(
    defaultRecordSourceLocation);
void recordSourceLocation(Node* n) {
  return record_source_location.load()(n);
}
void setRecordSourceLocation(void (*v)(Node*)) {
  record_source_location.store(v);
}

void defaultWarn(const std::string& str) {
  AT_WARN(str);
}
std::atomic<warn_fn_type> warn_callback{defaultWarn};

const char* WARN_PYTHON_DATAFLOW =
    " might cause the trace to be incorrect. We can't record the data flow of "
    "Python values, so this value will be treated as a constant in the future. "
    "This means that the trace might not generalize to other inputs!";
const char* WARN_CONSTRUCTOR =
    " results are registered as constants in the trace. You can safely ignore this "
    "warning if you use this function to create tensors out of constant variables "
    "that would be the same every time you call this function. In any other case, "
    "this might cause the trace to be incorrect.";
const char* WARN_RESIZE =
    " can't be represented in the JIT at the moment, so we won't connect any uses of "
    "this value with its current trace. If you happen to use it again, it will show "
    "up as a constant in the graph.";

// XXX: _kind can be a nullptr
void _do_warn(const char* _reason, const char* _kind) {
  std::string reason{_reason};
  std::string kind{_kind ? _kind : ""};
  std::ostringstream s;
  s << reason << kind;
  warn_callback.load()(s.str());
}

void setWarn(warn_fn_type fn) {
  warn_callback.store(fn);
}

} // namespace tracer
} // namespace jit
} // namespace torch
