#pragma once

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef USE_C10D_XCCL

#include <oneapi/ccl.hpp>
#include <torch/csrc/xpu/xccl.h>
#include <exception>
#include <memory>
#include <vector>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <torch/csrc/distributed/c10d/Backend.hpp>
#include <torch/csrc/distributed/c10d/NCCLUtils.hpp>
#include <torch/csrc/distributed/c10d/PrefixStore.hpp>
#include <torch/csrc/distributed/c10d/Store.hpp>
#include <torch/csrc/distributed/c10d/intra_node_comm.hpp>

namespace c10d {

constexpr const char* XCCL_BACKEND_NAME = "xccl";

class ProcessGroupXCCL : public Backend {
 public:
  class WorkXCCL : public Work {
   public:
    WorkXCCL(
        std::vector<std::vector<at::Tensor>> outputTensors,
        int rank = -1,
        OpType opType = UNKNOWN,
        const c10::optional<std::vector<at::Tensor>>& inputTensors =
            c10::nullopt)
        : Work(rank, opType), outputTensors_(std::move(outputTensors)) {}

    WorkXCCL(const WorkXCCL& w)
        : outputTensors_(w.outputTensors_), events_(w.events_) {}

    ~WorkXCCL() override {
      // Ensures all events are properly handled before destruction
      for (auto& event : events_) {
        event.wait();
      }
    }

    bool isCompleted() override {
      for (const auto& event : events_) {
        if (!event.test()) {
          return false;
        }
      }
      return true;
    }

    bool isSuccess() const override {
      TORCH_CHECK(
          false, "ProcessGroupXCCL::WorkXCCL::isSuccess not implemented");
    }

    void abort() override {
      TORCH_CHECK(false, "ProcessGroupXCCL::WorkXCCL::abort not implemented");
    }

    void synchronize() override {
      for (auto& event : events_) {
        event.wait();
      }
    }

    void wait() override {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& event : events_) {
        CCL_CHECK(event.wait());
      }
      events_.clear();
    }

    c10::intrusive_ptr<c10::ivalue::Future> getFuture() override {
      TORCH_CHECK(
          false, "ProcessGroupXCCL::WorkXCCL::getFuture not implemented");
    }

    std::vector<at::Tensor> result() override {
      return outputTensors_.empty() ? std::vector<at::Tensor>()
                                    : outputTensors_[0];
    }

   protected:
    friend class ProcessGroupXCCL;
    std::vector<ccl::event> events_;
    const std::vector<std::vector<at::Tensor>> outputTensors_;
    c10::intrusive_ptr<at::ivalue::Future> future_;
  };

  explicit ProcessGroupXCCL(
      const c10::intrusive_ptr<Store>& store,
      int rank,
      int size)
      : store_(store), rank_(rank), size_(size) {}

  ProcessGroupXCCL::~ProcessGroupXCCL() = default;

  const std::string getBackendName() const override {
    return std::string(XCCL_BACKEND_NAME);
  }

  c10::intrusive_ptr<Work> allreduce(
      std::vector<at::Tensor>& tensors,
      const AllreduceOptions& opts = AllreduceOptions()) override;

  c10::intrusive_ptr<Work> barrier(
      const BarrierOptions& opts = BarrierOptions()) override;

  static c10::intrusive_ptr<Backend> createProcessGroupXCCL(
      const c10::intrusive_ptr<Store>& store,
      int rank = -1,
      int size = -1);

 private:
  int rank_;
  int size_;

 public:
  std::unordered_map<std::string, std::shared_ptr<XCCLComm>>
      inInitializationCommMap_;
  std::unordered_map<std::string, std::shared_ptr<XCCLComm>> devXCCLCommMap_;
  c10::intrusive_ptr<Store> store_;
  std::mutex mutex_;
};

} // namespace c10d

#endif // USE_C10D_XCCL
