#include <c10d/FileStore.hpp>
#include <c10d/ProcessGroupGloo.hpp>

using namespace ::c10d;

int main(int argc, char** argv) {
  int rank = atoi(getenv("RANK"));
  int size = atoi(getenv("SIZE"));
  auto store = std::make_shared<FileStore>("/tmp/c10d_example", size);
  ProcessGroupGloo pg(store, rank, size);

  // Create some tensors
  const auto ntensors = 10;
  std::vector<at::Tensor> tensors;
  for (auto i = 0; i < ntensors; i++) {
    auto x =
        at::ones({1000, 16 * (i + 1)}, at::TensorOptions(at::CPU(at::kFloat)));
    tensors.push_back(x);
  }

  // Kick off work
  std::vector<std::shared_ptr<ProcessGroup::Work>> pending;
  for (auto i = 0; i < ntensors; i++) {
    std::vector<at::Tensor> tmp = {tensors[i]};
    pending.push_back(pg.allreduce(tmp));
  }

  // Wait for work to complete
  for (auto& work : pending) {
    work->wait();
  }
}
