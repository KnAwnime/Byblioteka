r"""
This package adds support for CUDA tensor types, that implement the same
function as CPU tensors, but they utilize GPUs for computation.

It is lazily initialized, so you can always import it, and use
:func:`is_available()` to determine if your system supports CUDA.

:ref:`cuda-semantics` has more details about working with CUDA.
"""

import contextlib
import os
import torch
import traceback
import warnings
import threading
from torch._six import raise_from
from ._utils import _get_device_index
import torch._C

try:
    from torch._C import _cudart
except ImportError:
    _cudart = None

_initialized = False
_tls = threading.local()
_initialization_lock = threading.Lock()
_queued_calls = []  # don't invoke these until initialization occurs
_is_in_bad_fork = getattr(torch._C, "_cuda_isInBadFork", lambda: False)


def is_available():
    r"""Returns a bool indicating if CUDA is currently available."""
    if (not hasattr(torch._C, '_cuda_isDriverSufficient') or
            not torch._C._cuda_isDriverSufficient()):
        return False
    return torch._C._cuda_getDeviceCount() > 0


def _sleep(cycles):
    torch._C._cuda_sleep(cycles)


def _check_driver():
    if not hasattr(torch._C, '_cuda_isDriverSufficient'):
        raise AssertionError("Torch not compiled with CUDA enabled")
    if not torch._C._cuda_isDriverSufficient():
        if torch._C._cuda_getDriverVersion() == 0:
            # found no NVIDIA driver on the system
            raise AssertionError("""
Found no NVIDIA driver on your system. Please check that you
have an NVIDIA GPU and installed a driver from
http://www.nvidia.com/Download/index.aspx""")
        else:
            # TODO: directly link to the alternative bin that needs install
            raise AssertionError("""
The NVIDIA driver on your system is too old (found version {}).
Please update your GPU driver by downloading and installing a new
version from the URL: http://www.nvidia.com/Download/index.aspx
Alternatively, go to: https://pytorch.org to install
a PyTorch version that has been compiled with your version
of the CUDA driver.""".format(str(torch._C._cuda_getDriverVersion())))


def _check_capability():
    incorrect_binary_warn = """
    Found GPU%d %s which requires CUDA_VERSION >= %d to
     work properly, but your PyTorch was compiled
     with CUDA_VERSION %d. Please install the correct PyTorch binary
     using instructions from https://pytorch.org
    """

    old_gpu_warn = """
    Found GPU%d %s which is of cuda capability %d.%d.
    PyTorch no longer supports this GPU because it is too old.
    The minimum cuda capability that we support is 3.5.
    """

    if torch.version.cuda is not None:  # on ROCm we don't want this check
        CUDA_VERSION = torch._C._cuda_getCompiledVersion()
        for d in range(device_count()):
            capability = get_device_capability(d)
            major = capability[0]
            minor = capability[1]
            name = get_device_name(d)
            if capability == (3, 0) or major < 3:
                warnings.warn(old_gpu_warn % (d, name, major, capability[1]))
            elif CUDA_VERSION <= 9000 and major >= 7 and minor >= 5:
                warnings.warn(incorrect_binary_warn % (d, name, 10000, CUDA_VERSION))


def is_initialized():
    r"""Returns whether PyTorch's CUDA state has been initialized."""
    return _initialized and not _is_in_bad_fork()


def _lazy_call(callable):
    if is_initialized():
        callable()
    else:
        # Don't store the actual traceback to avoid memory cycle
        _queued_calls.append((callable, traceback.format_stack()))

_lazy_call(_check_capability)


class DeferredCudaCallError(Exception):
    pass


def init():
    r"""Initialize PyTorch's CUDA state.  You may need to call
    this explicitly if you are interacting with PyTorch via
    its C API, as Python bindings for CUDA functionality will not
    be until this initialization takes place.  Ordinary users
    should not need this, as all of PyTorch's CUDA methods
    automatically initialize CUDA state on-demand.

    Does nothing if the CUDA state is already initialized.
    """
    _lazy_init()


def _lazy_init():
    global _initialized, _queued_calls
    if is_initialized() or hasattr(_tls, 'is_initializing'):
        return
    with _initialization_lock:
        # We be double-checked locking, boys!  This is OK because
        # the above test was GIL protected anyway.  The inner test
        # is for when a thread blocked on some other thread which was
        # doing the initialization; when they get the lock, they will
        # find there is nothing left to do.
        if is_initialized():
            return
        # It is important to prevent other threads from entering _lazy_init
        # immediately, while we are still guaranteed to have the GIL, because some
        # of the C calls we make below will release the GIL
        if _is_in_bad_fork():
            from sys import version_info
            if version_info < (3, 4):
                msg = ("To use CUDA with multiprocessing, you must use Python "
                       "3.4+ and the 'spawn' start method")
            else:
                msg = ("To use CUDA with multiprocessing, you must use the "
                       "'spawn' start method")
            raise RuntimeError(
                "Cannot re-initialize CUDA in forked subprocess. " + msg)
        _check_driver()
        if _cudart is None:
            raise AssertionError(
                "libcudart functions unavailable. It looks like you have a broken build?")
        torch._C._cuda_init()
        # Some of the queued calls may reentrantly call _lazy_init();
        # we need to just return without initializing in that case.
        # However, we must not let any *other* threads in!
        _tls.is_initializing = True
        try:
            for queued_call, orig_traceback in _queued_calls:
                try:
                    queued_call()
                except Exception as e:
                    msg = ("CUDA call failed lazily at initialization with error: {}\n\n"
                           "CUDA call was originally invoked at:\n\n{}").format(str(e), orig_traceback)
                    raise_from(DeferredCudaCallError(msg), e)
        finally:
            delattr(_tls, 'is_initializing')
        _initialized = True


def cudart():
    _lazy_init()
    return _cudart


class cudaStatus(object):
    SUCCESS = 0
    ERROR_NOT_READY = 34


class CudaError(RuntimeError):
    def __init__(self, code):
        msg = _cudart.cudaGetErrorString(code).decode('utf-8')
        super(CudaError, self).__init__('{0} ({1})'.format(msg, code))


def check_error(res):
    if res != _cudart.cudaError.success:
        raise CudaError(res)


class device(object):
    r"""Context-manager that changes the selected device.

    Arguments:
        device (torch.device or int): device index to select. It's a no-op if
            this argument is a negative integer or ``None``.
    """

    def __init__(self, device):
        self.idx = _get_device_index(device, optional=True)
        self.prev_idx = -1

    def __enter__(self):
        if self.idx == -1:
            return
        self.prev_idx = torch._C._cuda_getDevice()
        if self.prev_idx != self.idx:
            torch._C._cuda_setDevice(self.idx)
        _lazy_init()

    def __exit__(self, *args):
        if self.prev_idx != self.idx:
            torch._C._cuda_setDevice(self.prev_idx)
        return False


class device_of(device):
    r"""Context-manager that changes the current device to that of given object.

    You can use both tensors and storages as arguments. If a given object is
    not allocated on a GPU, this is a no-op.

    Arguments:
        obj (Tensor or Storage): object allocated on the selected device.
    """

    def __init__(self, obj):
        idx = obj.get_device() if obj.is_cuda else -1
        super(device_of, self).__init__(idx)


def set_device(device):
    r"""Sets the current device.

    Usage of this function is discouraged in favor of :any:`device`. In most
    cases it's better to use ``CUDA_VISIBLE_DEVICES`` environmental variable.

    Arguments:
        device (torch.device or int): selected device. This function is a no-op
            if this argument is negative.
    """
    device = _get_device_index(device)
    if device >= 0:
        torch._C._cuda_setDevice(device)


def get_device_name(device=None):
    r"""Gets the name of a device.

    Arguments:
        device (torch.device or int, optional): device for which to return the
            name. This function is a no-op if this argument is a negative
            integer. It uses the current device, given by :func:`~torch.cuda.current_device`,
            if :attr:`device` is ``None`` (default).
    """
    return get_device_properties(device).name


def get_device_capability(device=None):
    r"""Gets the cuda capability of a device.

    Arguments:
        device (torch.device or int, optional): device for which to return the
            device capability. This function is a no-op if this argument is
            a negative integer. It uses the current device, given by
            :func:`~torch.cuda.current_device`, if :attr:`device` is ``None``
            (default).

    Returns:
        tuple(int, int): the major and minor cuda capability of the device
    """
    prop = get_device_properties(device)
    return prop.major, prop.minor


def get_device_properties(device):
    _lazy_init()  # will define _get_device_properties and _CudaDeviceProperties
    device = _get_device_index(device, optional=True)
    if device < 0 or device >= device_count():
        raise AssertionError("Invalid device id")
    return _get_device_properties(device)


@contextlib.contextmanager
def stream(stream):
    r"""Context-manager that selects a given stream.

    All CUDA kernels queued within its context will be enqueued on a selected
    stream.

    Arguments:
        stream (Stream): selected stream. This manager is a no-op if it's
            ``None``.

    .. note:: Streams are per-device. If the selected stream is not on the
        current device, this function will also change the current device to
        match the stream.
    """
    if stream is None:
        yield
        return
    src_prev_stream = current_stream()

    if src_prev_stream.device != stream.device:
        # The given stream is on a different device; have to restore the
        # current_stream on that device on exit as well
        with device(stream.device):
            dst_prev_stream = current_stream()

    torch._C._cuda_setStream(stream._cdata)
    try:
        yield
    finally:
        if src_prev_stream.device != stream.device:
            torch._C._cuda_setStream(dst_prev_stream._cdata)
        torch._C._cuda_setStream(src_prev_stream._cdata)


def device_count():
    r"""Returns the number of GPUs available."""
    if is_available():
        return torch._C._cuda_getDeviceCount()
    else:
        return 0


def current_device():
    r"""Returns the index of a currently selected device."""
    _lazy_init()
    return torch._C._cuda_getDevice()


def synchronize(device=None):
    r"""Waits for all kernels in all streams on a CUDA device to complete.

    Arguments:
        device (torch.device or int, optional): device for which to synchronize.
            It uses the current device, given by :func:`~torch.cuda.current_device`,
            if :attr:`device` is ``None`` (default).
    """
    _lazy_init()
    with torch.cuda.device(device):
        return torch._C._cuda_synchronize()


def ipc_collect():
    r"""Force collects GPU memory after it has been released by CUDA IPC.

    .. note::
        Checks if any sent CUDA tensors could be cleaned from the memory. Force
        closes shared memory file used for reference counting if there is no
        active counters. Useful when the producer process stopped actively sending
        tensors and want to release unused memory.
    """
    _lazy_init()
    return torch._C._cuda_ipc_collect()


def current_stream(device=None):
    r"""Returns the currently selected :class:`Stream` for a given device.

    Arguments:
        device (torch.device or int, optional): selected device. Returns
            the currently selected :class:`Stream` for the current device, given
            by :func:`~torch.cuda.current_device`, if :attr:`device` is ``None``
            (default).
    """
    _lazy_init()
    return torch.cuda.Stream(_cdata=torch._C._cuda_getCurrentStream(
        _get_device_index(device, optional=True)))


def default_stream(device=None):
    r"""Returns the default :class:`Stream` for a given device.

    Arguments:
        device (torch.device or int, optional): selected device. Returns
            the default :class:`Stream` for the current device, given by
            :func:`~torch.cuda.current_device`, if :attr:`device` is ``None``
            (default).
    """
    _lazy_init()
    return torch.cuda.Stream(_cdata=torch._C._cuda_getDefaultStream(
        _get_device_index(device, optional=True)))


def current_blas_handle():
    r"""Returns cublasHandle_t pointer to current cuBLAS handle"""
    _lazy_init()
    return torch._C._cuda_getCurrentBlasHandle()


from .memory import *


from .random import *

################################################################################
# Define Storage and Tensor classes
################################################################################


from ..storage import _StorageBase


def _dummy_type(name):
    def init_err(self):
        class_name = self.__class__.__name__
        raise RuntimeError(
            "Tried to instantiate dummy base class {}".format(class_name))
    return type(storage_name, (object,), {"__init__": init_err})


if not hasattr(torch._C, 'CudaDoubleStorageBase'):
    # Define dummy base classes
    for t in ['Double', 'Float', 'Long', 'Int', 'Short', 'Char', 'Byte', 'Half', 'Bool', 'BFloat16',
              'ComplexDouble', 'ComplexFloat']:
        storage_name = 'Cuda{0}StorageBase'.format(t)
        tensor_name = 'Cuda{0}TensorBase'.format(t)

        torch._C.__dict__[storage_name] = _dummy_type(storage_name)
        torch._C.__dict__[tensor_name] = _dummy_type(tensor_name)

    torch._C.__dict__['_CudaStreamBase'] = _dummy_type('CudaStreamBase')
    torch._C.__dict__['_CudaEventBase'] = _dummy_type('CudaEventBase')


@staticmethod
def _lazy_new(cls, *args, **kwargs):
    _lazy_init()
    # We may need to call lazy init again if we are a forked child
    # del _CudaBase.__new__
    return super(_CudaBase, cls).__new__(cls, *args, **kwargs)


class _CudaBase(object):
    is_cuda = True
    is_sparse = False

    def type(self, *args, **kwargs):
        with device(self.get_device()):
            return super(_CudaBase, self).type(*args, **kwargs)

    __new__ = _lazy_new


class DoubleStorage(_CudaBase, torch._C.CudaDoubleStorageBase, _StorageBase):
    pass


class FloatStorage(_CudaBase, torch._C.CudaFloatStorageBase, _StorageBase):
    pass


class LongStorage(_CudaBase, torch._C.CudaLongStorageBase, _StorageBase):
    pass


class IntStorage(_CudaBase, torch._C.CudaIntStorageBase, _StorageBase):
    pass


class ShortStorage(_CudaBase, torch._C.CudaShortStorageBase, _StorageBase):
    pass


class CharStorage(_CudaBase, torch._C.CudaCharStorageBase, _StorageBase):
    pass


class ByteStorage(_CudaBase, torch._C.CudaByteStorageBase, _StorageBase):
    pass


class HalfStorage(_CudaBase, torch._C.CudaHalfStorageBase, _StorageBase):
    pass


class BoolStorage(_CudaBase, torch._C.CudaBoolStorageBase, _StorageBase):
    pass


class BFloat16Storage(_CudaBase, torch._C.CudaBFloat16StorageBase, _StorageBase):
    pass

class ComplexDoubleStorage(_CudaBase, torch._C.CudaComplexDoubleStorageBase, _StorageBase):
    pass


class ComplexFloatStorage(_CudaBase, torch._C.CudaComplexFloatStorageBase, _StorageBase):
    pass

torch._storage_classes.add(DoubleStorage)
torch._storage_classes.add(FloatStorage)
torch._storage_classes.add(LongStorage)
torch._storage_classes.add(IntStorage)
torch._storage_classes.add(ShortStorage)
torch._storage_classes.add(CharStorage)
torch._storage_classes.add(ByteStorage)
torch._storage_classes.add(HalfStorage)
torch._storage_classes.add(BoolStorage)
torch._storage_classes.add(BFloat16Storage)
torch._storage_classes.add(ComplexDoubleStorage)
torch._storage_classes.add(ComplexFloatStorage)

from . import sparse
from . import profiler
from . import nvtx
from .streams import Stream, Event
from . import amp
