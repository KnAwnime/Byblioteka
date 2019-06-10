#include <torch/csrc/jit/ir.h>
#include <torch/csrc/jit/script/schema_matching.h>
#include <torch/csrc/jit/script/sugared_value.h>
#include <torch/csrc/jit/script/tree_views.h>

namespace torch {
namespace jit {
namespace script {

struct NoneValue : SugaredValue {
  NoneValue() = default;
  std::string kind() const override {
    return "None";
  }
};

std::shared_ptr<SugaredValue> PrintValue::call(
    const SourceRange& loc,
    Function& m,
    at::ArrayRef<NamedValue> inputs,
    at::ArrayRef<NamedValue> attributes,
    size_t n_binders) {
  auto& g = *m.graph();
  if (!attributes.empty())
    throw ErrorReport(loc) << "print doesn't accept any keyword arguments";

  std::vector<Value*> lowered_inputs = toValues(*m.graph(), inputs);
  g.insertNode(g.create(prim::Print, lowered_inputs, 0)->setSourceRange(loc));
  return std::make_shared<NoneValue>();
}

static const std::unordered_map<std::string, std::string>&
builtin_cast_methods() {
  static std::unordered_map<std::string, std::string> builtin_cast_methods = {
      {"byte", "_cast_Byte"},
      {"char", "_cast_Char"},
      {"double", "_cast_Double"},
      {"float", "_cast_Float"},
      {"int", "_cast_Int"},
      {"long", "_cast_Long"},
      {"short", "_cast_Short"},
      {"half", "_cast_Half"}};
  return builtin_cast_methods;
}

// The current supported iterable/builtin function
static const std::unordered_set<c10::Symbol> iterable_funcs = {
  prim::range,
};

std::shared_ptr<SugaredValue> BuiltinFunction::call(
    const SourceRange& loc,
    Function& m,
    at::ArrayRef<NamedValue> inputs,
    at::ArrayRef<NamedValue> attributes,
    size_t n_binders) {
  // For builtin functions like range(), zip(), enumerate(), etc. We will emit
  // an IterableValue instead of inserting an op into the graph, this allow us
  // to handle the iterable efficiently in the compiler to fill in the loop info
  if (iterable_funcs.count(symbol)) {
    return std::make_shared<IterableValue>(symbol, toValues(*m.graph(), inputs));
  }
  return std::make_shared<SimpleValue>(
      emitBuiltinCall(loc, *m.graph(), symbol, self, inputs, attributes, true));
}

// support syntax sugar for x.foo(y, z) by allowing x.foo to return a
// callable value that will resolve to foo(x, y, z) when called.
std::shared_ptr<SugaredValue> SimpleValue::attr(
    const SourceRange& loc,
    Function& m,
    const std::string& field) {
  // Allow method-style casts on Tensor types. e.g. x.int()
  if (value_->type()->isSubtypeOf(TensorType::get())) {
    if (builtin_cast_methods().count(field)) {
      return std::make_shared<BuiltinFunction>(
          Symbol::aten(builtin_cast_methods().at(field)),
          NamedValue(loc, "self", value_));
    }
    // functions that are just direct property lookups on tensor
    // must be registered as prim::<name>(Tensor t) -> <return_type>
    static const std::unordered_set<std::string> fields = {
        "dtype",
        "device",
        "shape",
        "is_cuda",
        "requires_grad",
    };
    if (fields.count(field)) {
      auto r =
          m.graph()->insert(Symbol::fromQualString("prim::" + field), {value_});
      return std::make_shared<SimpleValue>(r);
    }
  }
  if (value_->type()->isSubtypeOf(NumberType::get())) {
    throw ErrorReport(loc) << "Cannot call methods on numbers";
  }
  if (auto tuple_type = value_->type()->cast<TupleType>()) {
    if (!tuple_type->hasNames()) {
      throw ErrorReport(loc) << "Getting attributes of tuples is not supported";
    }
    auto names = tuple_type->names();
    for (size_t i = 0; i < names.size(); i++) {
      if (names[i] == field) {
        auto idx = m.graph()->insertConstant(IValue(static_cast<int64_t>(i)));
        auto out_type = tuple_type->elements().at(i);
        auto r =
            m.graph()
                ->insertNode(m.graph()->createTupleIndex(value_, idx, out_type))
                ->output();
        return std::make_shared<SimpleValue>(r);
      }
    }
    throw ErrorReport(loc) << "Unknown attribute to named tuple";
  }

  if (auto classType = value_->type()->cast<ClassType>()) {
    // This is a class, emit the proper attribute lookup
    if (auto method = classType->getMethod(field)) {
      return std::make_shared<MethodValue>(getValue(), field);
    }
    if (!classType->hasAttribute(field)) {
      throw ErrorReport(loc)
          << "Tried to access to nonexistent attribute " << field
          << ". Did you forget to initialize it in __init__()?";
    }
    auto& g = *m.graph();
    auto n = g.insertNode(g.createGetAttr(value_, field));
    return std::make_shared<SimpleValue>(n->output());
  }

  return std::make_shared<BuiltinFunction>(
      Symbol::aten(field), NamedValue(loc, "self", value_));
}

std::vector<std::shared_ptr<SugaredValue>> SimpleValue::asTuple(
    const SourceRange& loc,
    Function& m,
    const c10::optional<size_t>& size_hint) {
  static const auto make_simple_value =
      [](Value* v) -> std::shared_ptr<SugaredValue> {
    return std::make_shared<SimpleValue>(v);
  };
  if (value_->type()->kind() == TypeKind::TupleType) {
    auto outputs = createTupleUnpack(value_);
    return fmap(outputs, make_simple_value);
  } else if (value_->type()->kind() == TypeKind::ListType) {
    if (!size_hint) {
      throw ErrorReport(loc)
          << "cannot statically infer the expected size of a "
          << "list in this context";
    }
    auto graph = value_->owningGraph();
    Node* unpack =
        graph->insertNode(graph->createListUnpack(value_, *size_hint));
    return fmap(unpack->outputs(), make_simple_value);
  }
  throw ErrorReport(loc) << value_->type()->python_str()
                         << " cannot be used as a tuple";
}

void SimpleValue::setAttr(
    const SourceRange& loc,
    Function& m,
    const std::string& field,
    Value* newValue) {
  const auto classType = value_->type()->cast<ClassType>();
  if (!classType) {
    throw ErrorReport(loc) << "Tried to set an attribute: " << field
                           << " on a non-class: "
                           << value_->type()->python_str();
  }
  auto expectedType = classType->getAttribute(field);
  if (!expectedType) {
    // If we are still compiling the __init__ method for this class, then
    // setting an unknown attribute adds it to the class's definition.

    // We are initializing if:
    const auto isInitializing =
        // 1. The method we're currently inserting into is an init method
        m.name() == "__init__" &&
        // 2. The `self` arg matches this value's type (i.e. we are in the init
        // method for this class, not some other class)
        !m.graph()->inputs().empty() &&
        m.graph()->inputs().at(0)->type() == classType;

    if (isInitializing) {
      classType->addAttribute(field, newValue->type());
      expectedType = newValue->type();

      const auto insertPoint = m.graph()->insertPoint();
      const auto topLevelBlock = m.graph()->block();
      if (insertPoint->owningBlock() != topLevelBlock) {
        throw ErrorReport(loc)
            << "First assignment cannot be in a control-flow block. "
            << "Initialize the field at the top level first.";
      }
    } else {
      throw ErrorReport(loc)
          << "Tried to set nonexistent attribute: " << field
          << ". Did you forget to initialize it in __init__()?";
    }
  }

  AT_ASSERT(expectedType);

  // Check type correctness
  const auto newType = newValue->type();
  if (!newType->isSubtypeOf(expectedType)) {
    throw ErrorReport(loc) << "Wrong type for attribute assignment. Expected "
                           << expectedType->python_str() << " but got "
                           << newType->python_str();
  }

  auto& g = *m.graph();
  g.insertNode(g.createSetAttr(value_, field, newValue));
}

std::shared_ptr<SugaredValue> SimpleValue::call(
    const SourceRange& loc,
    Function& m,
    at::ArrayRef<NamedValue> inputs,
    at::ArrayRef<NamedValue> attributes,
    size_t n_binders) {
  // allow our 'fake' closures to be called, used for fork serialization
  // at the moment, but can be expanded later
  Node* self = getValue()->node();
  if (self->kind() == prim::TupleConstruct && self->inputs().size() == 2 &&
      self->inputs().at(0)->node()->kind() == prim::Function) {
    std::shared_ptr<Graph> graph =
        self->inputs().at(0)->node()->g(attr::Subgraph);
    Value* context = self->inputs().at(1);
    AT_ASSERT(context->node()->kind() == prim::TupleConstruct);

    // fork nodes are emitted in their own block but we do not simplify
    // tuple construction across blocks. To ensure we clean up the tuple
    // construct create another copy of the tuple construct in the fork block
    Value* close_context =
        m.graph()
            ->insertNode(m.graph()->createTuple(context->node()->inputs()))
            ->output();
    auto fn = CompilationUnit().create_function("anon", graph);
    std::vector<NamedValue> ctx_inputs = {close_context};
    ctx_inputs.insert(ctx_inputs.end(), inputs.begin(), inputs.end());
    return FunctionValue(fn).call(loc, m, ctx_inputs, attributes, n_binders);
  }
  return SugaredValue::call(loc, m, inputs, attributes, n_binders);
}

std::vector<TypePtr> SimpleValue::getItersTypeInfo(
  const SourceRange& loc,
  Function& m) {
    TypePtr val_type = getValue()->type();
    if (auto list_type = val_type->cast<ListType>()) {
      return {list_type->getElementType()};
    } else if (val_type->isSubtypeOf(TensorType::get())) {
      return {val_type};
    } else {
      throw ErrorReport(loc)
          << "Value type " << val_type->str() << "cannot be used as a iterator";
    }
}

void SimpleValue::fillInLoopInfo(
  const SourceRange& loc,
  Function& m,
  Node* n,
  size_t iters_size) {
  TORCH_INTERNAL_ASSERT(n->kind() == prim::Loop);
  // List, Tuple, Tensor, fill in missing information desugaring
  Value* val = getValue();
  TypePtr val_type = val->type();
  Graph& g = *m.graph();

  // start insertion point right before Loop node
  WithInsertPoint guard(n);
  if (auto list_type = val_type->cast<ListType>()) {
    // fill in max_trip_count_val
    Value* max_trip_count_val = g.insert(aten::len, {val}, {}, loc);
    n->insertInput(0, max_trip_count_val);

    // fill in the target element assignment value in the beginning of the FOR loop
    {
      if (iters_size != 1) {
        throw ErrorReport(loc) <<"more than one iterable for in list";
      }
      Block* body_block = n->blocks()[0];
      // replace the first Placeholder node in the block with the correct assignment
      auto it = body_block->nodes().begin();
      Value* trip_count = body_block->inputs()[0]; // Iteration num
      TORCH_INTERNAL_ASSERT(it->kind() == prim::Placeholder);

      WithInsertPoint it_guard(*it);
      Value* cur_elem = g.insert(aten::select, {val, trip_count}, {}, loc);
      it->output()->replaceAllUsesWith(cur_elem);
      it.destroyCurrent();
    }
  } else if (val_type->isSubtypeOf(TensorType::get())) {
    // fill in max_trip_count_val
    Value* outermost_dim_index = g.insertConstant(0, IntType::get(), loc);
    // zero-dim tensor error handling
    Value* num_dim = g.insert(aten::dim, {val}, {}, loc);
    Value* cond_value = g.insert(aten::eq, {num_dim, outermost_dim_index}, {}, loc);
    Node* if_node = g.insertNode(g.create(prim::If, 0)->setSourceRange(loc));
    if_node->addInput(cond_value);

    Block* true_block = if_node->addBlock();
    if_node->addBlock();
    {
      WithInsertPoint guard(true_block);
      g.insert(prim::RaiseException,
          {std::string("iteration over a 0-d tensor!")}, {}, loc);
    }

    Value* sizes_tuple = g.insert(aten::size, {val}, {}, loc);
    Value* max_trip_count_val = g.insert(aten::select, {sizes_tuple, outermost_dim_index}, {}, loc);
    n->insertInput(0, max_trip_count_val);
    // fill in the target element assignment value in the beginning of the FOR loop
    {
      TORCH_CHECK(iters_size == 1, "more than one iterable for in tensor");
      Block* body_block = n->blocks()[0];
      // replace the first Placeholder node in the block with the correct assignment
      auto it = body_block->nodes().begin();
      Value* trip_count = body_block->inputs()[0]; // Iteration num
      TORCH_INTERNAL_ASSERT(it->kind() == prim::Placeholder);

      WithInsertPoint it_guard(*it);
      Value* cur_elem = g.insert(aten::select, {val, outermost_dim_index, trip_count}, {}, loc);
      it->output()->replaceAllUsesWith(cur_elem);
      it.destroyCurrent();
    }

  } else {
      throw ErrorReport(loc)
          << "Value type " << val_type->str() << "does not have loop information to fill";
    }
}

std::vector<TypePtr> IterableValue::getItersTypeInfo(
  const SourceRange& loc,
  Function& m) {
    if (symbol_ == prim::range) {
      return {IntType::get()};
    } else {
      throw ErrorReport(loc)
          << "No iterator type information on " << symbol_.toDisplayString();
    }
}

void IterableValue::fillInLoopInfo(
  const SourceRange& loc,
  Function& m,
  Node* n,
  size_t iters_size) {
  TORCH_INTERNAL_ASSERT(n->kind() == prim::Loop);
  // List, Tuple, Tensor, fill in missing information desugaring
  Graph& g = *m.graph();
  if (symbol_ == prim::range) {
    // fill in max_trip_count_val
    WithInsertPoint guard(n);
    Value* end_val = nullptr, *start_val = nullptr, *step_val = nullptr;
    Value* max_trip_count_val = nullptr;
    size_t iter_inputs_size = iter_inputs_.size();
    if (iter_inputs_size == 0) {
      throw ErrorReport(loc) << "range expected at least 1 arguments, got 0";
    } else if (iter_inputs_.size() == 1) {
      end_val = iter_inputs_[0];
      start_val = g.insertConstant(0, nullptr, loc);
      step_val = g.insertConstant(1, nullptr, loc);
      max_trip_count_val = end_val;
    } else if (iter_inputs_.size() <= 3) {
      start_val = iter_inputs_[0];
      end_val = iter_inputs_[1];
      if (iter_inputs_.size() == 3) {
        step_val = iter_inputs_[2];
        // error handling when step_val = 0 during runtime
        Value* cond_val = g.insert(aten::eq, {step_val, g.insertConstant(0, nullptr, loc)},{}, loc);
        Node* if_node = g.insertNode(g.create(prim::If, 0)->setSourceRange(loc));
        if_node->addInput(cond_val);
        auto true_block = if_node->addBlock();
        if_node->addBlock();
        WithInsertPoint guard(true_block);
        g.insert(prim::RaiseException,
            {std::string("range() arg 3 must not be zero")}, {}, loc);
      } else {
        step_val = g.insertConstant(1, nullptr, loc);
      }
      max_trip_count_val = g.insert(aten::__range_length, {start_val, end_val, step_val}, {}, loc);
    } else {
      throw ErrorReport(loc)
          << "range expected at most 3 arguments, got " << iter_inputs_size;
    }
    n->insertInput(0, max_trip_count_val);

    // fill in the target element assignment value in the beginning of the FOR loop
    {
      if (iters_size != 1) {
        throw ErrorReport(loc) <<"more than one iterable for in range";
      }
      Block* body_block = n->blocks()[0];
      // replace the first Placeholder node in the block with the correct assignment
      auto it = body_block->nodes().begin();
      Value* trip_count = body_block->inputs()[0]; // Iteration num
      TORCH_INTERNAL_ASSERT(it->kind() == prim::Placeholder);

      WithInsertPoint it_guard(*it);
      Value* cur_elem = trip_count;
      if (iter_inputs_size != 1) {
        cur_elem = g.insert(aten::__derive_index, {trip_count, start_val, step_val}, {}, loc);
      }
      it->output()->replaceAllUsesWith(cur_elem);
      it.destroyCurrent();
    }
  } else {
      throw ErrorReport(loc)
          << "Iterable " << symbol_.toDisplayString() << "does not have loop information to fill";
  }
}


std::shared_ptr<SugaredValue> ClassValue::call(
    const SourceRange& loc,
    Function& m,
    // note: names for args will be 'argument 0', 'argument 1', etc..
    at::ArrayRef<NamedValue> inputs,
    at::ArrayRef<NamedValue> attributes,
    size_t n_binders) {
  AT_ASSERT(n_binders <= 1);

  // Generate a new object of the right type, then call `__init__` on it
  auto& g = *m.graph();
  auto self = g.insertNode(g.createObject(type_))->output();

  // Call the init function
  MethodValue(self, "__init__").call(loc, m, inputs, attributes, n_binders);

  return std::make_shared<SimpleValue>(self);
}

std::shared_ptr<SugaredValue> ClassValue::attr(
    const SourceRange& loc,
    Function& m,
    const std::string& field) {
  if (field != "__new__") {
    throw ErrorReport(loc) << "Tried to lookup unknown attribute on class";
  }
  return std::make_shared<ClassNewMethod>(type_);
}
} // namespace script
} // namespace jit
} // namespace torch
