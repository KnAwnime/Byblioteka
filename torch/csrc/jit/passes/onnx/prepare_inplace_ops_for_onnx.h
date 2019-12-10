#pragma once

#include <torch/csrc/jit/ir.h>

namespace torch{
namespace jit{

TORCH_API void PrepareInplaceOpsForONNX(const std::shared_ptr<Graph>& graph);

} // namespace jit
} // namespace torch
