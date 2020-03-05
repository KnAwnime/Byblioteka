import os
import ctypes
import sys
import torch
import warnings
from torch.version import cuda
from contextlib import contextmanager
from subprocess import Popen, PIPE
from torch.backends import ContextProp, PropModule, __allow_nonbracketed_mutation

# Write:
#
#   torch.backends.cudnn.enabled = False
#
# to globally disable CuDNN/MIOpen

lib = None
__cudnn_version = None
# TODO: dynamic version checks via cudnnGetVersion

def find_cudnn_windows_lib():
    # Override the default search process
    # Fixes https://github.com/pytorch/pytorch/issues/20202
    # The library selection will be done in these directories one by one
    # 1. [Package Root]\Lib 
    #    That's where our libraries are in, which should be loaded first.
    # 2. Default directories
    #    That is stored in the environment variable `PATH`.
    test_env = os.environ.copy()
    old_path = test_env['PATH']
    th_dll_path = os.path.join(os.path.dirname(
        os.path.dirname(os.path.dirname(__file__))), 'lib')
    test_env['PATH'] = ';'.join([th_dll_path, old_path])
    proc = Popen(['where', 'cudnn64*.dll'], stdout=PIPE,
                 stderr=PIPE, stdin=PIPE, env=test_env)
    out, err = proc.communicate()
    out = out.decode().strip()
    if len(out) > 0:
        if out.find('\r\n') != -1:
            out = out.split('\r\n')[0]
        cudnn_lib = str(out)
        return ctypes.cdll.LoadLibrary(cudnn_lib)
    else:
        return None


def _libcudnn():
    global lib, __cudnn_version
    if lib is None:
        if sys.platform == "win32":
            lib = find_cudnn_windows_lib()
        else:
            lib = ctypes.cdll.LoadLibrary(None)
        if hasattr(lib, 'cudnnGetErrorString'):
            lib.cudnnGetErrorString.restype = ctypes.c_char_p
            __cudnn_version = lib.cudnnGetVersion()
            compile_version = torch._C._cudnn_version()
            # cuDNN version is MAJOR*1000 + MINOR*100 + PATCH
            runtime_major = __cudnn_version // 1000
            runtime_minor = (__cudnn_version % 1000) // 100
            compile_major = compile_version // 1000
            compile_minor = (compile_version % 1000) // 100
            # Different major versions are always incompatible
            # Starting with cuDNN 7, minor versions are backwards-compatible
            if runtime_major != compile_major:
                cudnn_compatible = False
            elif runtime_major < 7:
                cudnn_compatible = runtime_minor == compile_minor
            else:
                cudnn_compatible = runtime_minor >= compile_minor
            if not cudnn_compatible:
                raise RuntimeError(
                    'cuDNN version incompatibility: PyTorch was compiled against {} '
                    'but linked against {}'.format(compile_version, __cudnn_version))
        elif hasattr(lib, 'miopenGetVersion'):
            miopen_major = ctypes.c_size_t()
            miopen_minor = ctypes.c_size_t()
            miopen_patch = ctypes.c_size_t()
            # miopen version is MAJOR*1000000 + MINOR*1000 + PATCH
            lib.miopenGetVersion(ctypes.byref(miopen_major), ctypes.byref(miopen_minor), ctypes.byref(miopen_patch))
            __cudnn_version = miopen_major.value * 1000000 + miopen_minor.value * 1000 + miopen_patch.value
        else:
            lib = None
    return lib


def version():
    if _libcudnn() is None:
        return None
    return __cudnn_version


CUDNN_TENSOR_TYPES = {
    'torch.cuda.HalfTensor',
    'torch.cuda.FloatTensor',
    'torch.cuda.DoubleTensor',
}


def is_available():
    r"""Returns a bool indicating if CUDNN is currently available."""
    return torch._C.has_cudnn


def is_acceptable(tensor):
    if not torch._C._get_cudnn_enabled():
        return False
    if tensor.type() not in CUDNN_TENSOR_TYPES:
        return False
    if not is_available():
        warnings.warn(
            "PyTorch was compiled without cuDNN/MIOpen support. To use cuDNN/MIOpen, rebuild "
            "PyTorch making sure the library is visible to the build system.")
        return False
    if _libcudnn() is None:
        warnings.warn('cuDNN/MIOpen library not found. Check your {libpath}'.format(
            libpath={
                'darwin': 'DYLD_LIBRARY_PATH',
                'win32': 'PATH'
            }.get(sys.platform, 'LD_LIBRARY_PATH')))
        return False
    return True


_handles = {}

verbose = False

CUDNN_DATA_FLOAT = 0
CUDNN_DATA_DOUBLE = 1
CUDNN_DATA_HALF = 2

CUDNN_TENSOR_NCHW = 0
CUDNN_TENSOR_NHWC = 1

CUDNN_RNN_RELU = 0
CUDNN_RNN_TANH = 1
CUDNN_LSTM = 2
CUDNN_GRU = 3

CUDNN_LINEAR_INPUT = 0
CUDNN_SKIP_INPUT = 1

CUDNN_RNN_ALGO_STANDARD = 0
CUDNN_RNN_ALGO_PERSIST_STATIC = 1
CUDNN_RNN_ALGO_PERSIST_DYNAMIC = 2

CUDNN_DEFAULT_MATH = 0
CUDNN_TENSOR_OP_MATH = 1


def set_flags(_enabled, _benchmark, _deterministic, _verbose):
    global benchmark, deterministic, verbose
    orig_flags = (torch._C._get_cudnn_enabled(),
                  torch._C._get_cudnn_benchmark(),
                  torch._C._get_cudnn_deterministic(),
                  verbose)
    verbose = _verbose
    torch._C._set_cudnn_enabled(_enabled)
    torch._C._set_cudnn_benchmark(_benchmark)
    torch._C._set_cudnn_deterministic(_deterministic)
    return orig_flags

@contextmanager
def flags(enabled=False, benchmark=False, deterministic=False, verbose=False):
    with __allow_nonbracketed_mutation():
        orig_flags = set_flags(enabled, benchmark, deterministic, verbose)
    try:
        yield
    finally:
        # recover the previous values
        with __allow_nonbracketed_mutation():
            set_flags(orig_flags[0], orig_flags[1], orig_flags[2], orig_flags[3])


class CuDNNHandle:
    def __init__(self):
        ptr = ctypes.c_void_p()
        check_error(lib.cudnnCreate(ctypes.byref(ptr)))
        self._as_parameter_ = ptr

    def __del__(self):
        check_error(lib.cudnnDestroy(self))


class CuDNNError(RuntimeError):
    def __init__(self, status):
        self.status = status
        msg = '{}: {}'.format(status, get_error_string(status))
        super(CuDNNError, self).__init__(msg)


class TensorDescriptor(object):
    def __init__(self):
        ptr = ctypes.c_void_p()
        check_error(lib.cudnnCreateTensorDescriptor(ctypes.byref(ptr)))
        self._as_parameter_ = ptr

    def __del__(self):
        check_error(lib.cudnnDestroyTensorDescriptor(self._as_parameter_))
        del self._as_parameter_

    def set(self, tensor):
        self._type = tensor.type()
        self._size = tensor.size()
        self._stride = tensor.stride()
        check_error(lib.cudnnSetTensorNdDescriptor(
            self, _typemap[tensor.type()], tensor.dim(),
            int_array(tensor.size()), int_array(tensor.stride())))

    def as_tuple(self):
        return (self._type, tuple(self._size), tuple(self._stride))


class TensorDescriptorArray(object):
    def __init__(self, N):
        self.ptrs = (ctypes.c_void_p * N)()
        for i in range(N):
            ptr = ctypes.byref(self.ptrs, i * ctypes.sizeof(ctypes.c_void_p))
            check_error(lib.cudnnCreateTensorDescriptor(ptr))
        self._as_parameter_ = self.ptrs

    def __del__(self):
        for ptr in self.ptrs:
            check_error(lib.cudnnDestroyTensorDescriptor(ctypes.c_void_p(ptr)))

    def __getitem__(self, key):
        return ctypes.c_void_p(self.ptrs[key])

    def set_all(self, tensor):
        _type = _typemap[tensor.type()]
        _ndim = tensor.dim()
        _size = int_array(tensor.size())
        _stride = int_array(tensor.stride())
        for ptr in self.ptrs:
            check_error(lib.cudnnSetTensorNdDescriptor(
                ctypes.c_void_p(ptr), _type, _ndim, _size, _stride))

    def set_raw(self, i, _type, _ndim, _size, _stride):
        ptr = self.ptrs[i]
        check_error(lib.cudnnSetTensorNdDescriptor(
            ctypes.c_void_p(ptr), _type, _ndim, _size, _stride))


class FilterDescriptor(object):
    def __init__(self):
        ptr = ctypes.c_void_p()
        check_error(lib.cudnnCreateFilterDescriptor(ctypes.byref(ptr)))
        self._as_parameter_ = ptr

    def __del__(self):
        check_error(lib.cudnnDestroyFilterDescriptor(self._as_parameter_))
        del self._as_parameter_

    def set(self, weight):
        self._size = weight.size()
        datatype = _typemap[weight.type()]
        check_error(lib.cudnnSetFilterNdDescriptor(
            self, datatype, CUDNN_TENSOR_NCHW, weight.ndimension(),
            int_array(weight.size())))

    def as_tuple(self):
        return tuple(self._size)


class DropoutDescriptor(object):
    def __init__(self, handle, dropout, seed):
        ptr = ctypes.c_void_p()
        check_error(lib.cudnnCreateDropoutDescriptor(ctypes.byref(ptr)))

        self._as_parameter_ = ptr
        self.state = None
        self.dropout = dropout
        self.handle = handle

        self._set(dropout, seed)

    def set_dropout(self, dropout, seed):
        if dropout != self.dropout:
            self._set(dropout, seed)

    def _set(self, dropout, seed):
        if self.state is None and dropout > 0:
            dropout_states_size = ctypes.c_long()
            check_error(lib.cudnnDropoutGetStatesSize(
                self.handle,
                ctypes.byref(dropout_states_size)))
            self.state = torch.cuda.ByteTensor(dropout_states_size.value)
            state_ptr = self.state.data_ptr()
            state_size = self.state.size(0)
        else:
            state_ptr = None
            state_size = 0

        check_error(lib.cudnnSetDropoutDescriptor(
            self,
            self.handle,
            ctypes.c_float(dropout),
            ctypes.c_void_p(state_ptr),
            ctypes.c_size_t(state_size),
            ctypes.c_ulonglong(seed),
        ))

        self.dropout = dropout

    def __del__(self):
        check_error(lib.cudnnDestroyDropoutDescriptor(self))


class RNNDescriptor(object):
    def __init__(self, handle, hidden_size, num_layers, dropout_desc, input_mode,
                 bidirectional, mode, datatype):
        ptr = ctypes.c_void_p()
        check_error(lib.cudnnCreateRNNDescriptor(ctypes.byref(ptr)))
        self._as_parameter_ = ptr
        if version() >= 6000:
            check_error(lib.cudnnSetRNNDescriptor_v6(
                handle,
                self,
                hidden_size,
                num_layers,
                dropout_desc,
                input_mode,
                bidirectional,
                mode,
                CUDNN_RNN_ALGO_STANDARD,
                datatype
            ))
            if version() >= 7000 and int(cuda[0]) >= 9 and (
                    torch.cuda.get_device_capability(torch.cuda.current_device())[0] >= 7):
                lib.cudnnSetRNNMatrixMathType(self, CUDNN_DEFAULT_MATH)
                if datatype == CUDNN_DATA_HALF:
                    lib.cudnnSetRNNMatrixMathType(self, CUDNN_TENSOR_OP_MATH)
        else:
            check_error(lib.cudnnSetRNNDescriptor(
                self,
                hidden_size,
                num_layers,
                dropout_desc,
                input_mode,
                bidirectional,
                mode,
                datatype
            ))

    def __del__(self):
        check_error(lib.cudnnDestroyRNNDescriptor(self))


def check_error(status):
    if status != 0:
        raise CuDNNError(status)


def get_error_string(status):
    return lib.cudnnGetErrorString(status)


def get_handle():
    if _libcudnn() is None:
        raise RuntimeError('cuDNN not available')
    current_device = torch.cuda.current_device()
    handle = _handles.get(current_device, None)
    if handle is None:
        handle = CuDNNHandle()
        _handles[current_device] = handle
    return handle


_typemap = {
    'torch.cuda.HalfTensor': CUDNN_DATA_HALF,
    'torch.cuda.FloatTensor': CUDNN_DATA_FLOAT,
    'torch.cuda.DoubleTensor': CUDNN_DATA_DOUBLE,
}

_sizeofmap = {
    CUDNN_DATA_HALF: 2,
    CUDNN_DATA_FLOAT: 4,
    CUDNN_DATA_DOUBLE: 8,
}


def c_type(tensor):
    if isinstance(tensor, torch.cuda.HalfTensor):
        return ctypes.c_float
    elif isinstance(tensor, torch.cuda.FloatTensor):
        return ctypes.c_float
    elif isinstance(tensor, torch.cuda.DoubleTensor):
        return ctypes.c_double
    else:
        raise ValueError("unknown type '{}'".format(type(tensor)))


def int_array(itr):
    array_type = ctypes.c_int * len(itr)
    return array_type(*itr)


def descriptor(tensor, N=None):
    padded_size = tensor.size() + ((1,) * (5 - tensor.dim()))
    tensor = tensor.view(padded_size)
    if N is not None:
        descriptor = TensorDescriptorArray(N)
        descriptor.set_all(tensor)
    else:
        descriptor = TensorDescriptor()
        descriptor.set(tensor)
    return descriptor


def descriptor_sequence(tensor, batch_sizes):
    descriptors = TensorDescriptorArray(len(batch_sizes))
    _type = _typemap[tensor.type()]
    _ndim = 5
    dim_pad = (1,) * (5 - tensor.dim())
    _size = int_array(tensor.size() + dim_pad)
    _stride = int_array(tensor.stride() + dim_pad)
    for i, batch_size in enumerate(batch_sizes):
        _size[0] = batch_size
        descriptors.set_raw(i, _type, _ndim, _size, _stride)
    return descriptors


def add_tensor(*args):
    check_error(lib.cudnnAddTensor(*args))


# The magic here is to allow us to intercept code like this:
#
#   torch.backends.<cudnn|mkldnn>.enabled = True

class CudnnModule(PropModule):
    def __init__(self, m, name):
        super(CudnnModule, self).__init__(m, name)

    enabled = ContextProp(torch._C._get_cudnn_enabled, torch._C._set_cudnn_enabled)
    deterministic = ContextProp(torch._C._get_cudnn_deterministic, torch._C._set_cudnn_deterministic)
    benchmark = ContextProp(torch._C._get_cudnn_benchmark, torch._C._set_cudnn_benchmark)

# This is the sys.modules replacement trick, see
# https://stackoverflow.com/questions/2447353/getattr-on-a-module/7668273#7668273
sys.modules[__name__] = CudnnModule(sys.modules[__name__], __name__)
