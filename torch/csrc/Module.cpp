#include <Python.h>
#include <sys/types.h>

#ifndef _MSC_VER
#include <sys/socket.h>
#endif

#include <stdbool.h>
#include <unordered_map>
#include <libshm.h>
#include <TH/TH.h>
#include <ATen/ATen.h>
#include <ATen/dlpack.h>
#include <ATen/DLConvertor.h>

#include "torch/csrc/DynamicTypes.h"
#include "torch/csrc/autograd/generated/python_nn_functions.h"
#include "torch/csrc/utils/python_strings.h"
#include "torch/csrc/utils/tensor_numpy.h"
#include "torch/csrc/jit/python_tracer.h"
#include "torch/csrc/jit/init.h"
#include "torch/csrc/jit/python_ir.h"

#ifdef WITH_CUDNN
#include <ATen/cudnn/cudnn-wrapper.h>
#endif

#define WITH_NUMPY_IMPORT_ARRAY
#include "THP.h"

#include "ModuleSparse.cpp"
#include "DataLoader.cpp"

PyObject* module;
PyObject* tensor_classes;

PyObject *THPDefaultTensorClass = NULL;
THPGenerator *THPDefaultGenerator   = NULL;

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static bool THPModule_loadClasses(PyObject *self)
{
#define ASSERT_NOT_NULL(ptr) if (!(ptr)) { THPUtils_setError("couldn't load classes"); return false; }
  PyObject *torch_module = PyImport_ImportModule("torch");
  if (!torch_module) {
    THPUtils_setError("class loader couldn't access torch module");
    return false;
  }

  ASSERT_NOT_NULL(tensor_classes = PyObject_GetAttrString(torch_module, "_tensor_classes"));
  if (!THPDoubleTensor_postInit(torch_module)) return false;
  if (!THPFloatTensor_postInit(torch_module)) return false;
  if (!THPHalfTensor_postInit(torch_module)) return false;
  if (!THPLongTensor_postInit(torch_module)) return false;
  if (!THPIntTensor_postInit(torch_module)) return false;
  if (!THPShortTensor_postInit(torch_module)) return false;
  if (!THPCharTensor_postInit(torch_module)) return false;
  if (!THPByteTensor_postInit(torch_module)) return false;

  THPDoubleStorage_postInit(torch_module);
  THPFloatStorage_postInit(torch_module);
  THPHalfStorage_postInit(torch_module);
  THPLongStorage_postInit(torch_module);
  THPIntStorage_postInit(torch_module);
  THPShortStorage_postInit(torch_module);
  THPCharStorage_postInit(torch_module);
  THPByteStorage_postInit(torch_module);

  return true;
#undef ASSERT_NOT_NULL
}

static PyObject * THPModule_initNames(PyObject *self, PyObject *arg)
{
  static std::vector<std::string> names;

  THPObjectPtr types(PySequence_Fast(arg, "expected a sequence"));
  if (!types) return NULL;

  int num_classes = PySequence_Fast_GET_SIZE(types.get());
  names.reserve(names.size() + num_classes);
  for (int i = 0; i < num_classes; i++) {
    PyObject* obj = PySequence_Fast_GET_ITEM(types.get(), i);
    THPUtils_assert(PyType_Check(obj), "expected a PyTypeObject");
    PyTypeObject* type = (PyTypeObject*)obj;

    THPObjectPtr module_name(PyObject_GetAttrString(obj, "__module__"));
    if (!module_name) return NULL;
    THPUtils_assert(THPUtils_checkString(module_name.get()),
        "expected __module__ to be a string");
    std::string name = THPUtils_unpackString(module_name.get());
    names.push_back(name + "." + type->tp_name);
    type->tp_name = names.back().c_str();
  }
  Py_RETURN_NONE;
}

static bool THPModule_assignStateless(PyObject *self)
{
#define INIT_STATELESS(type)                                                   \
  stateless = PyObject_CallFunctionObjArgs((PyObject*)&TH_CONCAT_2(type, TensorStatelessType), NULL); \
  if (!stateless) {                                                            \
    return false;                                                              \
  }                                                                            \
  if (PyObject_SetAttrString(TH_CONCAT_3(THP,type,TensorClass), THP_STATELESS_ATTRIBUTE_NAME, stateless) == -1) { \
    return false;                                                              \
  }
  PyObject *stateless;
  INIT_STATELESS(Double);
  INIT_STATELESS(Float);
  INIT_STATELESS(Half);
  INIT_STATELESS(Long);
  INIT_STATELESS(Int);
  INIT_STATELESS(Short);
  INIT_STATELESS(Char);
  INIT_STATELESS(Byte);
  return true;
#undef INIT_STATELESS
}
//
// Callback for python part. Used for additional initialization of python classes
static PyObject * THPModule_initExtension(PyObject *self, PyObject *shm_manager_path)
{
  HANDLE_TH_ERRORS
  if (!THPUtils_checkString(shm_manager_path)) {
    THPUtils_setError("initialization error - expected bytes/string object as shm_manager_path!");
    return NULL;
  }
  std::string path = THPUtils_unpackString(shm_manager_path);
  libshm_init(path.c_str());
  if (!THPModule_loadClasses(self))         return NULL;
  if (!THPModule_assignStateless(self))     return NULL;
  if (!THPAutograd_initFunctions(self))     return NULL;
  Py_RETURN_NONE;
  END_HANDLE_TH_ERRORS
}

static PyObject * THPModule_getNumThreads(PyObject *module)
{
  return PyLong_FromLong(THGetNumThreads());
}

static PyObject * THPModule_setNumThreads(PyObject *module, PyObject *arg)
{
  THPUtils_assert(THPUtils_checkLong(arg), "set_num_threads expects an int, "
          "but got %s", THPUtils_typename(arg));
  THSetNumThreads((int)THPUtils_unpackLong(arg));
  Py_RETURN_NONE;
}

bool THPModule_isTensor(PyObject *obj)
{
  int result = PySet_Contains(tensor_classes, (PyObject*)Py_TYPE(obj));
  if (result == -1)
    throw std::logic_error("FATAL: tensor_classes isn't a set!");
  return result;
}

PyObject * THPModule_setDefaultTensorType(PyObject *_unused, PyObject *type)
{
  THPDefaultTensorClass = type;
  Py_RETURN_NONE;
}

PyObject * THPModule_fromNumpy(PyObject *_unused, PyObject *array)
{
  HANDLE_TH_ERRORS
  return torch::createPyObject(torch::utils::tensor_from_numpy(array));
  END_HANDLE_TH_ERRORS
}

/**
 * STATELESS FUNCTIONS
 **/

static PyObject * findTensor(PyObject *args, PyObject *kwargs) {
  for (Py_ssize_t i = 0; i < PyTuple_Size(args); i++) {
    PyObject *item = PyTuple_GET_ITEM(args, i);
    if (THPModule_isTensor(item) || THPVariable_Check(item)) {
      return item;
    }
  }
  if (kwargs) {
    Py_ssize_t pos = 0;
    PyObject *key, *value;
    while (PyDict_Next(kwargs, &pos, &key, &value)) {
      if (THPModule_isTensor(value) || THPVariable_Check(value)) {
        return value;
      }
    }
  }
  return THPDefaultTensorClass;
}

static PyObject * swapFirstTwoItems(PyObject *args) {
  // Returns a tuple with the first two items swapped
  auto size = PyTuple_GET_SIZE(args);
  auto r = THPObjectPtr{PyTuple_New(size)};
  if (!r) return nullptr;
  for (Py_ssize_t i = 0; i < size; i++) {
    PyObject* obj = PyTuple_GET_ITEM(args, (i <= 1 ? 1 - i : i));
    Py_INCREF(obj);
    PyTuple_SET_ITEM(r.get(), i, obj);
  }
  return r.release();
}

static PyObject * dispatchStateless(PyObject *args, PyObject *kwargs, const char *name) {
  PyObject *tensor = findTensor(args, kwargs);
  return THPUtils_dispatchStateless(tensor, name, args, kwargs);
}

static PyObject * dispatchStatelessAddXX(PyObject *args, PyObject *kwargs, const char *name) {
  PyObject *tensor = findTensor(args, kwargs);
  if (THPVariable_Check(tensor) && PyTuple_GET_SIZE(args) >= 2 && tensor == PyTuple_GET_ITEM(args, 1)) {
    // On Variables, swap the first two arguments if the 'self' argument comes
    // second. This handles the deprecated torch.addxx signatures. For example,
    // torch.addmm(1, var, 2, a, b) -> var.addmm(1, 2, a, b)
    auto newArgs = THPObjectPtr{swapFirstTwoItems(args)};
    return THPUtils_dispatchStateless(tensor, name, newArgs.get(), kwargs);
  } else {
    return THPUtils_dispatchStateless(tensor, name, args, kwargs);
  }
}

#define IMPLEMENT_STATELESS(name)                                              \
static PyObject * TH_CONCAT_2(THPModule_, name)(PyObject *_unused, PyObject *args, PyObject *kwargs) \
{                                                                              \
  return dispatchStateless(args, kwargs, #name);                               \
}

#define IMPLEMENT_STATELESS_ADDXX(name)                                        \
static PyObject * TH_CONCAT_2(THPModule_, name)(PyObject *_unused, PyObject *args, PyObject *kwargs) \
{                                                                              \
  return dispatchStatelessAddXX(args, kwargs, #name);                          \
}

IMPLEMENT_STATELESS(sigmoid)
IMPLEMENT_STATELESS(log)
IMPLEMENT_STATELESS(log1p)
IMPLEMENT_STATELESS(lgamma)
IMPLEMENT_STATELESS(erf)
IMPLEMENT_STATELESS(erfinv)
IMPLEMENT_STATELESS(exp)
IMPLEMENT_STATELESS(cos)
IMPLEMENT_STATELESS(acos)
IMPLEMENT_STATELESS(cosh)
IMPLEMENT_STATELESS(sin)
IMPLEMENT_STATELESS(asin)
IMPLEMENT_STATELESS(sinh)
IMPLEMENT_STATELESS(tan)
IMPLEMENT_STATELESS(atan)
IMPLEMENT_STATELESS(tanh)
IMPLEMENT_STATELESS(sqrt)
IMPLEMENT_STATELESS(rsqrt)
IMPLEMENT_STATELESS(ceil)
IMPLEMENT_STATELESS(floor)
IMPLEMENT_STATELESS(round)
IMPLEMENT_STATELESS(abs)
IMPLEMENT_STATELESS(trunc)
IMPLEMENT_STATELESS(frac)
IMPLEMENT_STATELESS(mean)
IMPLEMENT_STATELESS(std)
IMPLEMENT_STATELESS(var)
IMPLEMENT_STATELESS(norm)
IMPLEMENT_STATELESS(reciprocal)
IMPLEMENT_STATELESS(neg)
IMPLEMENT_STATELESS(add)
IMPLEMENT_STATELESS(mul)
IMPLEMENT_STATELESS(div)
IMPLEMENT_STATELESS(fmod)
IMPLEMENT_STATELESS(min)
IMPLEMENT_STATELESS(max)
IMPLEMENT_STATELESS(dot)
IMPLEMENT_STATELESS(sum)
IMPLEMENT_STATELESS(prod)
IMPLEMENT_STATELESS(remainder)
IMPLEMENT_STATELESS(cumsum)
IMPLEMENT_STATELESS(cumprod)
IMPLEMENT_STATELESS(clamp)
IMPLEMENT_STATELESS(equal)
IMPLEMENT_STATELESS(eye)
IMPLEMENT_STATELESS(diag)
IMPLEMENT_STATELESS(numel)
IMPLEMENT_STATELESS(sign)
IMPLEMENT_STATELESS(trace)
IMPLEMENT_STATELESS(tril)
IMPLEMENT_STATELESS(triu)
IMPLEMENT_STATELESS(zero)
IMPLEMENT_STATELESS(kthvalue)
IMPLEMENT_STATELESS(mode)
IMPLEMENT_STATELESS(median)
IMPLEMENT_STATELESS(cross)
IMPLEMENT_STATELESS(sort)
IMPLEMENT_STATELESS(topk)
IMPLEMENT_STATELESS(t)
IMPLEMENT_STATELESS(transpose)
IMPLEMENT_STATELESS(squeeze)
IMPLEMENT_STATELESS(unsqueeze)
IMPLEMENT_STATELESS(renorm)
IMPLEMENT_STATELESS(dist)
IMPLEMENT_STATELESS(linspace)
IMPLEMENT_STATELESS(logspace)
IMPLEMENT_STATELESS(histc)
IMPLEMENT_STATELESS(atan2)
IMPLEMENT_STATELESS(pow)
IMPLEMENT_STATELESS(lerp)
IMPLEMENT_STATELESS(zeros)
IMPLEMENT_STATELESS(zeros_like)
IMPLEMENT_STATELESS(ones)
IMPLEMENT_STATELESS(ones_like)
IMPLEMENT_STATELESS(index_select)
IMPLEMENT_STATELESS(take)
IMPLEMENT_STATELESS(ger)
IMPLEMENT_STATELESS(mv)
IMPLEMENT_STATELESS(mm)
IMPLEMENT_STATELESS(bmm)
// TODO: this doesn't implement options that return numbers!
IMPLEMENT_STATELESS(multinomial)
IMPLEMENT_STATELESS(normal)
IMPLEMENT_STATELESS(standard_gamma)
IMPLEMENT_STATELESS(standard_gamma_grad)
IMPLEMENT_STATELESS(bernoulli)
IMPLEMENT_STATELESS(range)
IMPLEMENT_STATELESS(arange)
IMPLEMENT_STATELESS(gather)
IMPLEMENT_STATELESS(rand)
IMPLEMENT_STATELESS(randn)
IMPLEMENT_STATELESS(masked_select)
IMPLEMENT_STATELESS(gesv)
IMPLEMENT_STATELESS(gels)
IMPLEMENT_STATELESS(trtrs)
IMPLEMENT_STATELESS(symeig)
IMPLEMENT_STATELESS(eig)
IMPLEMENT_STATELESS(svd)
IMPLEMENT_STATELESS(inverse)
IMPLEMENT_STATELESS(potrf)
IMPLEMENT_STATELESS(potrs)
IMPLEMENT_STATELESS(potri)
IMPLEMENT_STATELESS(pstrf)
IMPLEMENT_STATELESS(qr)
IMPLEMENT_STATELESS(geqrf)
IMPLEMENT_STATELESS(orgqr)
IMPLEMENT_STATELESS(ormqr)
IMPLEMENT_STATELESS(btrifact)
IMPLEMENT_STATELESS(btrisolve)
IMPLEMENT_STATELESS(gt)
IMPLEMENT_STATELESS(lt)
IMPLEMENT_STATELESS(ge)
IMPLEMENT_STATELESS(le)
IMPLEMENT_STATELESS(eq)
IMPLEMENT_STATELESS(ne)

IMPLEMENT_STATELESS_ADDXX(addmm)
IMPLEMENT_STATELESS_ADDXX(addmv)
IMPLEMENT_STATELESS_ADDXX(addr)
IMPLEMENT_STATELESS_ADDXX(addbmm)
IMPLEMENT_STATELESS_ADDXX(baddbmm)
IMPLEMENT_STATELESS_ADDXX(addcmul)
IMPLEMENT_STATELESS_ADDXX(addcdiv)

#undef IMPLEMENT_STATELESS
#undef IMPLEMENT_STATELESS_ADDXX

// In nonzero, the first argument might be a LongTensor that will be used
// for indices output, so we should pick a function based on second
// tensor's type.
static PyObject * THPModule_nonzero(PyObject *_unused, PyObject *args, PyObject *kwargs)
{
  PyObject *tensor = THPDefaultTensorClass;
  if (PyTuple_Size(args) == 1)
    tensor = PyTuple_GET_ITEM(args, 0);
  else if (PyTuple_Size(args) == 2)
    tensor = PyTuple_GET_ITEM(args, 1);
  return THPUtils_dispatchStateless(tensor, "nonzero", args, kwargs);
}

static PyObject * THPModule_randperm(PyObject *_unused, PyObject *args, PyObject *kwargs)
{
  PyObject *tensor = THPLongTensorClass;
  PyObject *out;
  if (kwargs && (out = PyDict_GetItemString(kwargs, "out")))
      tensor = out;
  return THPUtils_dispatchStateless(tensor, "randperm", args, kwargs);
}

static PyObject * THPModule_cat(PyObject *_unused, PyObject *args, PyObject *kwargs)
{
  PyObject *tensor = THPDefaultTensorClass;
  THPObjectPtr iterator;
  THPObjectPtr item;
  PyObject *first_arg=nullptr;
  if (args && PyTuple_GET_SIZE(args) > 0) {
    first_arg = PyTuple_GET_ITEM(args, 0);
  } else if (kwargs && PyTuple_GET_ITEM(args, 0)) {
    first_arg = PyDict_GetItemString(kwargs, "seq");
  }

  if (first_arg) {
    if (THPModule_isTensor(first_arg)) {
      tensor = first_arg;
    } else if (PySequence_Check(first_arg)) {
      item = PySequence_GetItem(first_arg, 0);
      if (item && (THPModule_isTensor(item) || THPVariable_Check(item))) {
        tensor = item;
      }
    }
    PyErr_Clear();
  }

  return THPUtils_dispatchStateless(tensor, "cat", args, kwargs);
}

PyObject *THPModule_safeCall(PyObject *_unused, PyObject *args, PyObject *kwargs)
{
  PyObject *result = NULL;
  PyObject *args_slice = NULL;
  PyThreadState *thread_state = PyThreadState_Get();
  Py_ssize_t num_args = args ? PyTuple_Size(args) : 0;
  THPUtils_assert(num_args > 0, "expected at least one argument");
  try {
    args_slice = PyTuple_GetSlice(args, 1, num_args);
    result = PyObject_Call(PyTuple_GET_ITEM(args, 0), args_slice, kwargs);
  } catch (std::exception &e) {
    PyEval_RestoreThread(thread_state);
    Py_DECREF(args_slice);
    PyErr_SetString(THPException_FatalError, e.what());
    Py_LeaveRecursiveCall();
  }
  Py_DECREF(args_slice);
  return result;
}

PyObject *THPModule_addDocStr(PyObject *_unused, PyObject *args)
{
  // adds a __doc__ string to a function, similar to numpy's arr_add_docstring
  static std::vector<std::string> all_docs;
  PyObject *obj;
  PyObject *doc_obj;
  if (!PyArg_ParseTuple(args, "OO", &obj, &doc_obj)) {
    return NULL;
  }

  const char* doc_str = "<invalid string>";
  if (THPUtils_checkString(doc_obj)) {
    all_docs.push_back(THPUtils_unpackString(doc_obj));
    doc_str = all_docs.back().c_str();
  }

  if (Py_TYPE(obj) == &PyCFunction_Type) {
    PyCFunctionObject* f = (PyCFunctionObject *)obj;
    if (f->m_ml->ml_doc) {
      return PyErr_Format(PyExc_RuntimeError,
          "function '%s' already has a docstring", f->m_ml->ml_name);
    }
    f->m_ml->ml_doc = doc_str;
  } else if (strcmp(Py_TYPE(obj)->tp_name, "method_descriptor") == 0) {
    PyMethodDescrObject* m = (PyMethodDescrObject *)obj;
    if (m->d_method->ml_doc) {
      return PyErr_Format(PyExc_RuntimeError,
          "method '%s' already has a docstring", m->d_method->ml_name);
    }
    m->d_method->ml_doc = doc_str;
  } else {
    return PyErr_Format(PyExc_TypeError,
        "don't know how to add docstring to type '%s'", Py_TYPE(obj)->tp_name);
  }

  Py_INCREF(obj);
  return obj;
}


PyObject *THPModule_inferSize(PyObject *_unused, PyObject *args)
{
  HANDLE_TH_ERRORS
  Py_ssize_t num_args = args ? (Py_ssize_t) PyTuple_Size(args) : 0;
  THPUtils_assert(num_args == 2, "expected exactly 2 arguments");
  PyObject *arg1 = PyTuple_GET_ITEM(args, 0);
  THPUtils_assert(THPSize_Check(arg1), "expected a torch.Size as argument 1");
  PyObject *arg2 = PyTuple_GET_ITEM(args, 1);
  THPUtils_assert(THPSize_Check(arg2), "expected a torch.Size as argument 2");

  THLongStoragePtr size1_guard = THPUtils_unpackSize(arg1);
  THLongStorage *size1 = size1_guard.get();
  THLongStoragePtr size2_guard = THPUtils_unpackSize(arg2);
  THLongStorage *size2 = size2_guard.get();
  THLongStoragePtr sizes_guard(THLongStorage_new());
  THLongStorage *sizes = sizes_guard.get();

  char error_buffer[1024];
  int ret = THLongStorage_inferSize2(sizes, size1->data, size1->size, size2->data, size2->size, error_buffer, 1024);
  THPUtils_assert(ret == 0, error_buffer);
  return THPSize_New(sizes->size, sizes->data);
  END_HANDLE_TH_ERRORS
}

static PyObject *THPModule_setBackcompatBroadcastWarn(PyObject *module, PyObject *arg) {
  THPUtils_assert(PyBool_Check(arg), "set_backcompat_broadcast_warn expects a bool, "
          "but got %s", THPUtils_typename(arg));
  setBackCompatBroadcastWarn(arg == Py_True);
  Py_RETURN_NONE;
}

static PyObject *THPModule_getBackcompatBroadcastWarn(PyObject *module)
{
  if (getBackCompatBroadcastWarn()) Py_RETURN_TRUE;
  else Py_RETURN_FALSE;
}

static PyObject *THPModule_setBackcompatKeepdimWarn(PyObject *module, PyObject *arg) {
  THPUtils_assert(PyBool_Check(arg), "set_backcompat_keepdim_warn expects a bool, "
          "but got %s", THPUtils_typename(arg));
  setBackCompatKeepdimWarn(arg == Py_True);
  Py_RETURN_NONE;
}

static PyObject *THPModule_getBackcompatKeepdimWarn(PyObject *module)
{
  if (getBackCompatKeepdimWarn()) Py_RETURN_TRUE;
  else Py_RETURN_FALSE;
}

PyObject *THPModule_hasDistributed(PyObject *_unused)
{
#ifdef WITH_DISTRIBUTED
  Py_RETURN_TRUE;
#else
  Py_RETURN_FALSE;
#endif
}

PyObject *THPModule_toDLPack(PyObject *_unused, PyObject *data)
{
  THPUtils_assert(THPModule_isTensor(data), "data must be a Tensor");
  auto atTensor = torch::createTensor(data);
  DLManagedTensor* dlMTensor = at::toDLPack(atTensor);
  return PyCapsule_New(dlMTensor, "dltensor", NULL);
}

PyObject *THPModule_fromDLPack(PyObject *_unused, PyObject *data)
{
  DLManagedTensor * dlMTensor = (DLManagedTensor *)PyCapsule_GetPointer(data, "dltensor");
  THPUtils_assert(dlMTensor, "from_dlpack received an invalid capsule. "
    "Note that DLTensor capsules can be consumed only once, "
    "so you might have already constructed a tensor from it once.")
  // atensor steals the ownership of the underlying storage. It also passes a
  // destructor function that will be called when the underlying storage goes
  // out of scope. When the destructor is called, the dlMTensor is destructed too.
  at::Tensor atensor = at::fromDLPack(dlMTensor);
  // Make sure this capsule will never be used again.
  PyCapsule_SetName(data, "used_dltensor");
  return torch::createPyObject(atensor);
}

PyObject *THPModule_setUserEnabledCuDNN(PyObject *_unused, PyObject *arg)
{
  THPUtils_assert(PyBool_Check(arg), "set_enabled_cudnn expects a bool, "
          "but got %s", THPUtils_typename(arg));
  at::globalContext().setUserEnabledCuDNN(arg == Py_True);
  Py_RETURN_NONE;
}

PyObject *THPModule_userEnabledCuDNN(PyObject *_unused)
{
  if (at::globalContext().userEnabledCuDNN()) Py_RETURN_TRUE;
  else Py_RETURN_FALSE;
}

#ifdef WITH_CUDA
extern PyObject * THCSPModule_initExtension(PyObject *self);
#endif

static PyMethodDef TorchMethods[] = {
  {"_initExtension",  (PyCFunction)THPModule_initExtension,   METH_O,       NULL},
  {"_autograd_init",  (PyCFunction)THPAutograd_initExtension, METH_NOARGS,  NULL},
  {"_add_docstr",     (PyCFunction)THPModule_addDocStr,       METH_VARARGS, NULL},
  {"_sparse_init",    (PyCFunction)THSPModule_initExtension,  METH_NOARGS,  NULL},
  {"_init_names",     (PyCFunction)THPModule_initNames,       METH_O,       NULL},
  {"_has_distributed",(PyCFunction)THPModule_hasDistributed,  METH_NOARGS,  NULL},
#ifdef WITH_CUDA
  {"_cuda_sparse_init",  (PyCFunction)THCSPModule_initExtension,    METH_NOARGS,  NULL},
#endif
  {"_safe_call",      (PyCFunction)THPModule_safeCall,          METH_VARARGS | METH_KEYWORDS, NULL},
  {"_set_default_tensor_type", (PyCFunction)THPModule_setDefaultTensorType, METH_O, NULL},
  {"_infer_size",     (PyCFunction)THPModule_inferSize,         METH_VARARGS, NULL},
  {"_set_backcompat_broadcast_warn", (PyCFunction)THPModule_setBackcompatBroadcastWarn, METH_O, NULL},
  {"_get_backcompat_broadcast_warn", (PyCFunction)THPModule_getBackcompatBroadcastWarn, METH_NOARGS, NULL},
  {"_set_backcompat_keepdim_warn", (PyCFunction)THPModule_setBackcompatKeepdimWarn, METH_O, NULL},
  {"_get_backcompat_keepdim_warn", (PyCFunction)THPModule_getBackcompatKeepdimWarn, METH_NOARGS, NULL},
  {"get_num_threads", (PyCFunction)THPModule_getNumThreads,     METH_NOARGS,  NULL},
  {"set_num_threads", (PyCFunction)THPModule_setNumThreads,     METH_O,       NULL},
  {"_get_cudnn_enabled", (PyCFunction)THPModule_userEnabledCuDNN, METH_NOARGS,     NULL},
  {"_set_cudnn_enabled", (PyCFunction)THPModule_setUserEnabledCuDNN, METH_O,  NULL},
  {"from_numpy",      (PyCFunction)THPModule_fromNumpy,         METH_O,       NULL},
  {"_to_dlpack",      (PyCFunction)THPModule_toDLPack,          METH_O,       NULL},
  {"_from_dlpack",    (PyCFunction)THPModule_fromDLPack,        METH_O,       NULL},

  {"sigmoid",         (PyCFunction)THPModule_sigmoid,           METH_VARARGS | METH_KEYWORDS, NULL},
  {"log",             (PyCFunction)THPModule_log,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"log1p",           (PyCFunction)THPModule_log1p,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"lgamma",          (PyCFunction)THPModule_lgamma,            METH_VARARGS | METH_KEYWORDS, NULL},
  {"erf",             (PyCFunction)THPModule_erf,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"erfinv",          (PyCFunction)THPModule_erfinv,            METH_VARARGS | METH_KEYWORDS, NULL},
  {"exp",             (PyCFunction)THPModule_exp,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"cos",             (PyCFunction)THPModule_cos,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"acos",            (PyCFunction)THPModule_acos,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"cosh",            (PyCFunction)THPModule_cosh,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"sin",             (PyCFunction)THPModule_sin,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"asin",            (PyCFunction)THPModule_asin,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"sinh",            (PyCFunction)THPModule_sinh,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"tan",             (PyCFunction)THPModule_tan,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"atan",            (PyCFunction)THPModule_atan,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"tanh",            (PyCFunction)THPModule_tanh,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"sqrt",            (PyCFunction)THPModule_sqrt,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"rsqrt",           (PyCFunction)THPModule_rsqrt,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"ceil",            (PyCFunction)THPModule_ceil,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"floor",           (PyCFunction)THPModule_floor,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"round",           (PyCFunction)THPModule_round,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"abs",             (PyCFunction)THPModule_abs,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"trunc",           (PyCFunction)THPModule_trunc,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"frac",            (PyCFunction)THPModule_frac,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"mean",            (PyCFunction)THPModule_mean,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"std",             (PyCFunction)THPModule_std,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"var",             (PyCFunction)THPModule_var,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"norm",            (PyCFunction)THPModule_norm,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"reciprocal",      (PyCFunction)THPModule_reciprocal,        METH_VARARGS | METH_KEYWORDS, NULL},
  {"neg",             (PyCFunction)THPModule_neg,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"add",             (PyCFunction)THPModule_add,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"mul",             (PyCFunction)THPModule_mul,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"div",             (PyCFunction)THPModule_div,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"fmod",            (PyCFunction)THPModule_fmod,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"min",             (PyCFunction)THPModule_min,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"max",             (PyCFunction)THPModule_max,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"dot",             (PyCFunction)THPModule_dot,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"sum",             (PyCFunction)THPModule_sum,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"prod",            (PyCFunction)THPModule_prod,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"remainder",       (PyCFunction)THPModule_remainder,         METH_VARARGS | METH_KEYWORDS, NULL},
  {"cumsum",          (PyCFunction)THPModule_cumsum,            METH_VARARGS | METH_KEYWORDS, NULL},
  {"cumprod",         (PyCFunction)THPModule_cumprod,           METH_VARARGS | METH_KEYWORDS, NULL},
  {"clamp",           (PyCFunction)THPModule_clamp,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"equal",           (PyCFunction)THPModule_equal,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"eye",             (PyCFunction)THPModule_eye,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"diag",            (PyCFunction)THPModule_diag,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"numel",           (PyCFunction)THPModule_numel,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"sign",            (PyCFunction)THPModule_sign,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"trace",           (PyCFunction)THPModule_trace,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"tril",            (PyCFunction)THPModule_tril,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"triu",            (PyCFunction)THPModule_triu,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"zero",            (PyCFunction)THPModule_zero,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"gt",              (PyCFunction)THPModule_gt,                METH_VARARGS | METH_KEYWORDS, NULL},
  {"lt",              (PyCFunction)THPModule_lt,                METH_VARARGS | METH_KEYWORDS, NULL},
  {"ge",              (PyCFunction)THPModule_ge,                METH_VARARGS | METH_KEYWORDS, NULL},
  {"le",              (PyCFunction)THPModule_le,                METH_VARARGS | METH_KEYWORDS, NULL},
  {"eq",              (PyCFunction)THPModule_eq,                METH_VARARGS | METH_KEYWORDS, NULL},
  {"ne",              (PyCFunction)THPModule_ne,                METH_VARARGS | METH_KEYWORDS, NULL},
  {"kthvalue",        (PyCFunction)THPModule_kthvalue,          METH_VARARGS | METH_KEYWORDS, NULL},
  {"mode",            (PyCFunction)THPModule_mode,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"median",          (PyCFunction)THPModule_median,            METH_VARARGS | METH_KEYWORDS, NULL},
  {"cross",           (PyCFunction)THPModule_cross,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"sort",            (PyCFunction)THPModule_sort,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"topk",            (PyCFunction)THPModule_topk,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"t",               (PyCFunction)THPModule_t,                 METH_VARARGS | METH_KEYWORDS, NULL},
  {"transpose",       (PyCFunction)THPModule_transpose,         METH_VARARGS | METH_KEYWORDS, NULL},
  {"squeeze",         (PyCFunction)THPModule_squeeze,           METH_VARARGS | METH_KEYWORDS, NULL},
  {"unsqueeze",       (PyCFunction)THPModule_unsqueeze,         METH_VARARGS | METH_KEYWORDS, NULL},
  {"nonzero",         (PyCFunction)THPModule_nonzero,           METH_VARARGS | METH_KEYWORDS, NULL},
  {"renorm",          (PyCFunction)THPModule_renorm,            METH_VARARGS | METH_KEYWORDS, NULL},
  {"dist",            (PyCFunction)THPModule_dist,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"linspace",        (PyCFunction)THPModule_linspace,          METH_VARARGS | METH_KEYWORDS, NULL},
  {"logspace",        (PyCFunction)THPModule_logspace,          METH_VARARGS | METH_KEYWORDS, NULL},
  {"histc",           (PyCFunction)THPModule_histc,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"atan2",           (PyCFunction)THPModule_atan2,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"pow",             (PyCFunction)THPModule_pow,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"lerp",            (PyCFunction)THPModule_lerp,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"zeros",           (PyCFunction)THPModule_zeros,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"zeros_like",      (PyCFunction)THPModule_zeros_like,        METH_VARARGS | METH_KEYWORDS, NULL},
  {"ones",            (PyCFunction)THPModule_ones,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"ones_like",       (PyCFunction)THPModule_ones_like,         METH_VARARGS | METH_KEYWORDS, NULL},
  {"index_select",    (PyCFunction)THPModule_index_select,      METH_VARARGS | METH_KEYWORDS, NULL},
  {"take",            (PyCFunction)THPModule_take,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"addmm",           (PyCFunction)THPModule_addmm,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"addmv",           (PyCFunction)THPModule_addmv,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"addr",            (PyCFunction)THPModule_addr,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"ger",             (PyCFunction)THPModule_ger,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"mv",              (PyCFunction)THPModule_mv,                METH_VARARGS | METH_KEYWORDS, NULL},
  {"addbmm",          (PyCFunction)THPModule_addbmm,            METH_VARARGS | METH_KEYWORDS, NULL},
  {"baddbmm",         (PyCFunction)THPModule_baddbmm,           METH_VARARGS | METH_KEYWORDS, NULL},
  {"addcmul",         (PyCFunction)THPModule_addcmul,           METH_VARARGS | METH_KEYWORDS, NULL},
  {"addcdiv",         (PyCFunction)THPModule_addcdiv,           METH_VARARGS | METH_KEYWORDS, NULL},
  {"mm",              (PyCFunction)THPModule_mm,                METH_VARARGS | METH_KEYWORDS, NULL},
  {"bmm",             (PyCFunction)THPModule_bmm,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"multinomial",     (PyCFunction)THPModule_multinomial,       METH_VARARGS | METH_KEYWORDS, NULL},
  {"normal",          (PyCFunction)THPModule_normal,            METH_VARARGS | METH_KEYWORDS, NULL},
  {"_standard_gamma",  (PyCFunction)THPModule_standard_gamma,    METH_VARARGS | METH_KEYWORDS, NULL},
  {"_standard_gamma_grad", (PyCFunction)THPModule_standard_gamma_grad, METH_VARARGS | METH_KEYWORDS, NULL},
  {"bernoulli",       (PyCFunction)THPModule_bernoulli,         METH_VARARGS | METH_KEYWORDS, NULL},
  {"rand",            (PyCFunction)THPModule_rand,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"randn",           (PyCFunction)THPModule_randn,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"randperm",        (PyCFunction)THPModule_randperm,          METH_VARARGS | METH_KEYWORDS, NULL},
  {"range",           (PyCFunction)THPModule_range,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"arange",          (PyCFunction)THPModule_arange,            METH_VARARGS | METH_KEYWORDS, NULL},
  {"gather",          (PyCFunction)THPModule_gather,            METH_VARARGS | METH_KEYWORDS, NULL},
  {"cat",             (PyCFunction)THPModule_cat,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"masked_select",   (PyCFunction)THPModule_masked_select,     METH_VARARGS | METH_KEYWORDS, NULL},
  {"gesv",            (PyCFunction)THPModule_gesv,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"gels",            (PyCFunction)THPModule_gels,              METH_VARARGS | METH_KEYWORDS, NULL},
  {"trtrs",           (PyCFunction)THPModule_trtrs,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"symeig",          (PyCFunction)THPModule_symeig,            METH_VARARGS | METH_KEYWORDS, NULL},
  {"eig",             (PyCFunction)THPModule_eig,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"svd",             (PyCFunction)THPModule_svd,               METH_VARARGS | METH_KEYWORDS, NULL},
  {"inverse",         (PyCFunction)THPModule_inverse,           METH_VARARGS | METH_KEYWORDS, NULL},
  {"potrf",           (PyCFunction)THPModule_potrf,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"potrs",           (PyCFunction)THPModule_potrs,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"potri",           (PyCFunction)THPModule_potri,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"pstrf",           (PyCFunction)THPModule_pstrf,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"qr",              (PyCFunction)THPModule_qr,                METH_VARARGS | METH_KEYWORDS, NULL},
  {"geqrf",           (PyCFunction)THPModule_geqrf,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"orgqr",           (PyCFunction)THPModule_orgqr,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"ormqr",           (PyCFunction)THPModule_ormqr,             METH_VARARGS | METH_KEYWORDS, NULL},
  {"btrifact",        (PyCFunction)THPModule_btrifact,          METH_VARARGS | METH_KEYWORDS, NULL},
  {"btrisolve",       (PyCFunction)THPModule_btrisolve,         METH_VARARGS | METH_KEYWORDS, NULL},

  // Sparse functions
  {"smm",             (PyCFunction)THSPModule_sspmm,          METH_VARARGS | METH_KEYWORDS,  NULL},
  {"saddmm",          (PyCFunction)THSPModule_sspaddmm,       METH_VARARGS | METH_KEYWORDS,  NULL},
  {"dsmm",            (PyCFunction)THSPModule_spmm,           METH_VARARGS | METH_KEYWORDS,  NULL},
  {"hsmm",            (PyCFunction)THSPModule_hspmm,          METH_VARARGS | METH_KEYWORDS,  NULL},
  {NULL, NULL, 0, NULL}
};

bool THCPDoubleStorage_init(PyObject *module);
bool THCPFloatStorage_init(PyObject *module);
bool THCPHalfStorage_init(PyObject *module);
bool THCPLongStorage_init(PyObject *module);
bool THCPIntStorage_init(PyObject *module);
bool THCPShortStorage_init(PyObject *module);
bool THCPCharStorage_init(PyObject *module);
bool THCPByteStorage_init(PyObject *module);

bool THCPDoubleTensor_init(PyObject *module);
bool THCPFloatTensor_init(PyObject *module);
bool THCPHalfTensor_init(PyObject *module);
bool THCPLongTensor_init(PyObject *module);
bool THCPIntTensor_init(PyObject *module);
bool THCPShortTensor_init(PyObject *module);
bool THCPCharTensor_init(PyObject *module);
bool THCPByteTensor_init(PyObject *module);

bool THCPStream_init(PyObject *module);

#ifdef WITH_CUDA
PyMethodDef* THCPModule_methods();
#endif

bool THCSPDoubleTensor_init(PyObject *module);
bool THCSPFloatTensor_init(PyObject *module);
bool THCSPHalfTensor_init(PyObject *module);
bool THCSPLongTensor_init(PyObject *module);
bool THCSPIntTensor_init(PyObject *module);
bool THCSPShortTensor_init(PyObject *module);
bool THCSPCharTensor_init(PyObject *module);
bool THCSPByteTensor_init(PyObject *module);

bool THDPDoubleStorage_init(PyObject *module);
bool THDPFloatStorage_init(PyObject *module);
//bool THDPHalfStorage_init(PyObject *module);
bool THDPLongStorage_init(PyObject *module);
bool THDPIntStorage_init(PyObject *module);
bool THDPShortStorage_init(PyObject *module);
bool THDPCharStorage_init(PyObject *module);
bool THDPByteStorage_init(PyObject *module);

bool THDPDoubleTensor_init(PyObject *module);
bool THDPFloatTensor_init(PyObject *module);
//bool THDPHalfTensor_init(PyObject *module);
bool THDPLongTensor_init(PyObject *module);
bool THDPIntTensor_init(PyObject *module);
bool THDPShortTensor_init(PyObject *module);
bool THDPCharTensor_init(PyObject *module);
bool THDPByteTensor_init(PyObject *module);

static std::vector<PyMethodDef> methods;

#ifdef WITH_DISTRIBUTED
PyMethodDef* THDPModule_methods();
#endif

// TODO: Refactor this in some less manual way
#ifdef WITH_CUDNN
static PyObject * THCUDNN_cudnn_version(PyObject *self, PyObject *args)
{
  return PyLong_FromLong(CUDNN_VERSION);
}

static PyMethodDef _THCUDNN_methods[] = {
  {"_cudnn_version", (PyCFunction)THCUDNN_cudnn_version, METH_VARARGS, NULL},
  {NULL}
};

PyMethodDef* THCUDNN_methods() {
  return _THCUDNN_methods;
}
#endif

static PyObject* initModule() {
  HANDLE_TH_ERRORS
  THInferNumThreads();

#define ASSERT_TRUE(cmd) if (!(cmd)) return NULL

  THPUtils_addPyMethodDefs(methods, TorchMethods);
  THPUtils_addPyMethodDefs(methods, DataLoaderMethods);
#ifdef WITH_CUDA
  THPUtils_addPyMethodDefs(methods, THCPModule_methods());
#endif
#ifdef WITH_CUDNN
  THPUtils_addPyMethodDefs(methods, THCUDNN_methods());
#endif
#ifdef WITH_DISTRIBUTED
  THPUtils_addPyMethodDefs(methods, THDPModule_methods());
#endif

#if PY_MAJOR_VERSION == 2
  ASSERT_TRUE(module = Py_InitModule("torch._C", methods.data()));
#else
  static struct PyModuleDef torchmodule = {
     PyModuleDef_HEAD_INIT,
     "torch._C",
     NULL,
     -1,
     methods.data()
  };
  ASSERT_TRUE(module = PyModule_Create(&torchmodule));
#endif
  ASSERT_TRUE(THPWrapper_init(module));
  ASSERT_TRUE(THPGenerator_init(module));
  ASSERT_TRUE(THPException_init(module));
  ASSERT_TRUE(THPSize_init(module));
  ASSERT_TRUE(THPVariable_initModule(module));
  ASSERT_TRUE(THPFunction_initModule(module));
  ASSERT_TRUE(THPEngine_initModule(module));
  torch::autograd::initAutogradClosureBindings(module);
  torch::jit::initJITBindings(module);
  torch::autograd::initNNFunctions(module);
  ASSERT_TRUE(THPDoubleStorage_init(module));
  ASSERT_TRUE(THPFloatStorage_init(module));
  ASSERT_TRUE(THPHalfStorage_init(module));
  ASSERT_TRUE(THPLongStorage_init(module));
  ASSERT_TRUE(THPIntStorage_init(module));
  ASSERT_TRUE(THPShortStorage_init(module));
  ASSERT_TRUE(THPCharStorage_init(module));
  ASSERT_TRUE(THPByteStorage_init(module));

  ASSERT_TRUE(THPDoubleTensor_init(module));
  ASSERT_TRUE(THPFloatTensor_init(module));
  ASSERT_TRUE(THPHalfTensor_init(module));
  ASSERT_TRUE(THPLongTensor_init(module));
  ASSERT_TRUE(THPIntTensor_init(module));
  ASSERT_TRUE(THPShortTensor_init(module));
  ASSERT_TRUE(THPCharTensor_init(module));
  ASSERT_TRUE(THPByteTensor_init(module));

  ASSERT_TRUE(THSPDoubleTensor_init(module));
  ASSERT_TRUE(THSPFloatTensor_init(module));
  ASSERT_TRUE(THSPLongTensor_init(module));
  ASSERT_TRUE(THSPIntTensor_init(module));
  ASSERT_TRUE(THSPShortTensor_init(module));
  ASSERT_TRUE(THSPCharTensor_init(module));
  ASSERT_TRUE(THSPByteTensor_init(module));

#ifdef WITH_CUDA
  // This will only initialise base classes and attach them to library namespace
  // They won't be ready for real usage until importing cuda module, that will
  // complete the process (but it defines Python classes before calling back into
  // C, so these lines have to execute first)..
  ASSERT_TRUE(THCPDoubleStorage_init(module));
  ASSERT_TRUE(THCPFloatStorage_init(module));
  ASSERT_TRUE(THCPHalfStorage_init(module));
  ASSERT_TRUE(THCPLongStorage_init(module));
  ASSERT_TRUE(THCPIntStorage_init(module));
  ASSERT_TRUE(THCPShortStorage_init(module));
  ASSERT_TRUE(THCPCharStorage_init(module));
  ASSERT_TRUE(THCPByteStorage_init(module));

  ASSERT_TRUE(THCPDoubleTensor_init(module));
  ASSERT_TRUE(THCPFloatTensor_init(module));
  ASSERT_TRUE(THCPHalfTensor_init(module));
  ASSERT_TRUE(THCPLongTensor_init(module));
  ASSERT_TRUE(THCPIntTensor_init(module));
  ASSERT_TRUE(THCPShortTensor_init(module));
  ASSERT_TRUE(THCPCharTensor_init(module));
  ASSERT_TRUE(THCPByteTensor_init(module));

  ASSERT_TRUE(THCPStream_init(module));

  ASSERT_TRUE(THCSPDoubleTensor_init(module));
  ASSERT_TRUE(THCSPFloatTensor_init(module));
  ASSERT_TRUE(THCSPHalfTensor_init(module));
  ASSERT_TRUE(THCSPLongTensor_init(module));
  ASSERT_TRUE(THCSPIntTensor_init(module));
  ASSERT_TRUE(THCSPShortTensor_init(module));
  ASSERT_TRUE(THCSPCharTensor_init(module));
  ASSERT_TRUE(THCSPByteTensor_init(module));
#endif

#ifdef WITH_CUDNN
  PyObject *has_cudnn = Py_True;
#else
  PyObject *has_cudnn = Py_False;
#endif
  Py_INCREF(has_cudnn);
  ASSERT_TRUE(PyModule_AddObject(module, "has_cudnn", has_cudnn) == 0);

#ifdef WITH_DISTRIBUTED_MW
  // See comment on CUDA objects
  ASSERT_TRUE(THDPDoubleStorage_init(module));
  ASSERT_TRUE(THDPFloatStorage_init(module));
  //ASSERT_TRUE(THDPHalfStorage_init(module));
  ASSERT_TRUE(THDPLongStorage_init(module));
  ASSERT_TRUE(THDPIntStorage_init(module));
  ASSERT_TRUE(THDPShortStorage_init(module));
  ASSERT_TRUE(THDPCharStorage_init(module));
  ASSERT_TRUE(THDPByteStorage_init(module));

  ASSERT_TRUE(THDPDoubleTensor_init(module));
  ASSERT_TRUE(THDPFloatTensor_init(module));
  //ASSERT_TRUE(THDPHalfTensor_init(module));
  ASSERT_TRUE(THDPLongTensor_init(module));
  ASSERT_TRUE(THDPIntTensor_init(module));
  ASSERT_TRUE(THDPShortTensor_init(module));
  ASSERT_TRUE(THDPCharTensor_init(module));
  ASSERT_TRUE(THDPByteTensor_init(module));
#endif

  // force ATen to initialize because it handles
  // setting up TH Errors so that they throw C++ exceptions
  at::init();

  auto& defaultGenerator = at::globalContext().defaultGenerator(at::kCPU);
  THPDefaultGenerator = (THPGenerator*)THPGenerator_NewWithGenerator(
    defaultGenerator);
  ASSERT_TRUE(PyModule_AddObject(module, "default_generator", (PyObject*)THPDefaultGenerator) == 0);

#ifdef WITH_NUMPY
  if (_import_array() < 0) return NULL;
#endif

  return module;
  END_HANDLE_TH_ERRORS
}

#if PY_MAJOR_VERSION == 2
PyMODINIT_FUNC init_C()
#else
PyMODINIT_FUNC PyInit__C()
#endif
{
#if PY_MAJOR_VERSION == 2
  initModule();
#else
  return initModule();
#endif
}
