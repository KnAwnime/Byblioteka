#include <torch/csrc/jit/fuser/cuda/manager.h>
#include <torch/csrc/jit/fuser/cuda/parser.h>
#include <torch/csrc/jit/fuser/common/tensor.h>
#include <torch/csrc/jit/fuser/common/fusion.h>
#include <torch/csrc/jit/fuser/common/utils.h>
#include <torch/csrc/jit/passes/canonicalize.h>

#include <ATen/cuda/CUDAContext.h>

#include <unordered_map>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

namespace {

struct CudaKernelEntry {
  int16_t device_;
  CUmodule module_;
  CUfunction function_;

  // TODO: we don't need to keep the whole Fusion around after compilation.
  Fusion fusion_;

};

// The reason for two unordered_map here is to cache `torch::jit::Graph` lowering
class CudaFusionManager {
public:
  static CudaFusionManager& getManager() {
    static CudaFusionManager cuda_fusion_manager_;
    return cuda_fusion_manager_;
  };

  // TODO: discuss whether or not we want reuse codegen.
  // TODO: I'm assuming we have stride information in `graph->toString`
  //       We need to make sure stride information is in the final string, as we
  //       want to AVOID kernel reuse between different fusion_node, unless they
  //       have identical contiguity information! (So identical stride + shape
  //       is even more restricting in a good way)
  int32_t registerOrGetCacheId(std::shared_ptr<Graph>& graph) {
    std::lock_guard<std::mutex> guard(mutex_);
    // prepare graph for lowering;
    Canonicalize(graph, false);
    //EraseShapeInformation(graph);
    auto repr = graph->toString(false);

    // create new graph_cache_ entry;
    if (graph_cache_.count(repr) == 0) {
      int32_t kernel_id = getNextUniqueID();

      graph_cache_[repr] = kernel_id;

      // default constructor via accessing empty key;
      // lower torch::jit::Graph to torch::jit::fuser::cuda::fusion
      parseJitIR(graph, kernel_cache_[kernel_id].fusion_);

      // TODO: compile and blablabla;
      //compileJitIR(fusion, kernel_cache_[kernel_id]);

      return kernel_id;
    } else {
      return graph_cache_[repr];
    }
  };

  // TODO: IO construction should go outside;
  void runFusionNode(
      int32_t kernel_id,
      const at::ArrayRef<IValue> inputs,
      std::vector<at::Tensor> outputs) {
    assert(kernel_cache_.count(kernel_id) != 0);

    CudaKernelEntry& cuda_kernel_entry = kernel_cache_[kernel_id];
    const Fusion& fusion = cuda_kernel_entry.fusion_;
    std::cout << "executing cached kernel: " << kernel_id << std::endl <<
        &fusion << std::endl;

    for (auto& output : outputs) {
      output.fill_(kernel_id+0.1234);
    }

  }

protected:

  std::mutex mutex_;

  void runCudaKernel(
    int32_t key,
    const std::vector<int>& contiguity_tag,
    const c10::Device) {
  };

  int32_t getNextUniqueID() {
    return next_unique_id_++;
  };

  std::unordered_map<std::string, int32_t> graph_cache_;
  std::unordered_map<int64_t, CudaKernelEntry> kernel_cache_;

private:

  int32_t next_unique_id_ = 0;
};


} // namespace

void compileCudaFusionGroup(Node* fusion_node) {
  assert(fusion_node->kind() == prim::FusionGroup);
  if (fusion_node->hasAttribute(attr::cache_id)) {
    // TODO: maybe we should error out here;
    AT_WARN("Double registration of CudaFusionGroup on CudaFusionManager");
  }
  int32_t fusion_cache_id = 
      CudaFusionManager::getManager().registerOrGetCacheId(fusion_node->g(attr::Subgraph));
  fusion_node->i_(attr::cache_id, fusion_cache_id);
}

void runCudaFusionGroup(const Node* const fusion_node, Stack& stack) {
  assert(fusion_node->kind() == prim::FusionGroup);
  // TODO: should we support runtime compilation with updated dynamic shape;
  //       shape inference would be needed so we can allocate output;
  assert(fusion_node->hasAttribute(attr::cache_id));
  int32_t kernel_id = fusion_node->i(attr::cache_id);

  // Currently we just construct I/O tensors for static graph;
  const std::shared_ptr<Graph> graph = fusion_node->g(attr::Subgraph);
  const auto nInputs = graph->inputs().size();
  at::ArrayRef<IValue> inputs = last(stack, nInputs);

  // we need to construct outputs;
  std::vector<at::Tensor> outputs;
  for (const auto* const output : graph->outputs()) {
    auto type = output->type()->expect<TensorType>();
    // Expect output to be tensor;
    assert(type && type->isComplete());

    const auto device = *(type->device());
    const auto scalar_type = *(type->scalarType());

    auto options = at::TensorOptions()
      .dtype(scalar_type)
      .layout(at::kStrided)
      .device(device)
      .requires_grad(type->requires_grad());

    const auto sizes = extractSizes(type);
    const auto strides= extractStrides(type);

    auto tensor = at::empty_strided(sizes, strides, options);
    outputs.push_back(tensor);
  }

  CudaFusionManager::getManager().runFusionNode(kernel_id, inputs, outputs);

  drop(stack, inputs.size());
  stack.insert(stack.end(),
      std::make_move_iterator(outputs.begin()),
      std::make_move_iterator(outputs.end()));
}

}}}} // namespace torch::jit::fuser::cuda
