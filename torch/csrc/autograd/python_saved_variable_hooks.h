#pragma once

#include <pybind11/pybind11.h>
#include <torch/csrc/autograd/python_variable.h>
#include <torch/csrc/autograd/saved_variable_hooks.h>
#include <torch/csrc/python_headers.h>
#include <torch/csrc/THP_export.h>
#include <ATen/ATen.h>

namespace py = pybind11;

namespace torch { namespace autograd {

struct PySavedVariableHooks : public SavedVariableHooks {
  PySavedVariableHooks(py::function &pack_hook, py::function &unpack_hook);
  void call_pack_hook(at::Tensor &tensor) override;
  at::Tensor call_unpack_hook() override;
  ~PySavedVariableHooks() override;

private:
  PyObject* pack_hook_;
  PyObject* unpack_hook_;
  PyObject* data_ = nullptr;
};

}}
