from __future__ import print_function
import torch
import contextlib

if torch._C._cuda_isDriverSufficient() == False:
    # TODO: directly link to the alternative binary that the user has to install
    raise AssertionError("The NVIDIA driver on your system is too old. It is of version: "
          + str(torch._C._cuda_getDriverVersion()) + "\n" +
          "Please update your GPU driver by downloading and installing " +
          " a new version from the URL: http://www.nvidia.com/Download/index.aspx "
          + "\nAlternatively, go to https://pytorch.org/binaries to install a "
          + "PyTorch version that is compiled for old NVIDIA drivers")


from torch.Storage import _StorageBase
from torch.Tensor import _TensorBase

################################################################################
# Define Storage and Tensor classes
################################################################################

class DoubleStorage(torch._C.CudaDoubleStorageBase, _StorageBase):
    pass
class FloatStorage(torch._C.CudaFloatStorageBase, _StorageBase):
    pass
class LongStorage(torch._C.CudaLongStorageBase, _StorageBase):
    pass
class IntStorage(torch._C.CudaIntStorageBase, _StorageBase):
    pass
class ShortStorage(torch._C.CudaShortStorageBase, _StorageBase):
    pass
class CharStorage(torch._C.CudaCharStorageBase, _StorageBase):
    pass
class ByteStorage(torch._C.CudaByteStorageBase, _StorageBase):
    pass
class HalfStorage(torch._C.CudaHalfStorageBase, _StorageBase):
    pass

class DoubleTensor(torch._C.CudaDoubleTensorBase, _TensorBase):
    def is_signed(self):
        return True
class FloatTensor(torch._C.CudaFloatTensorBase, _TensorBase):
    def is_signed(self):
        return True
class LongTensor(torch._C.CudaLongTensorBase, _TensorBase):
    def is_signed(self):
        return True
class IntTensor(torch._C.CudaIntTensorBase, _TensorBase):
    def is_signed(self):
        return True
class ShortTensor(torch._C.CudaShortTensorBase, _TensorBase):
    def is_signed(self):
        return True
class CharTensor(torch._C.CudaCharTensorBase, _TensorBase):
    def is_signed(self):
        # TODO
        return False
class ByteTensor(torch._C.CudaByteTensorBase, _TensorBase):
    def is_signed(self):
        return False
class HalfTensor(torch._C.CudaHalfTensorBase, _TensorBase):
    def is_signed(self):
        return True

torch._storage_classes.add(DoubleStorage)
torch._storage_classes.add(FloatStorage)
torch._storage_classes.add(LongStorage)
torch._storage_classes.add(IntStorage)
torch._storage_classes.add(ShortStorage)
torch._storage_classes.add(CharStorage)
torch._storage_classes.add(ByteStorage)

torch._tensor_classes.add(DoubleTensor)
torch._tensor_classes.add(FloatTensor)
torch._tensor_classes.add(LongTensor)
torch._tensor_classes.add(IntTensor)
torch._tensor_classes.add(ShortTensor)
torch._tensor_classes.add(CharTensor)
torch._tensor_classes.add(ByteTensor)

@contextlib.contextmanager
def device(idx):
    prev_idx = torch._C._cuda_getDevice()
    torch._C._cuda_setDevice(idx)
    yield
    torch._C._cuda_setDevice(prev_idx)

@contextlib.contextmanager
def _dummy_ctx():
    yield

def _tensor_cuda(self, idx=None):
    # This already is a CUDA tensor.
    # Let's check if it needs to be transfered to another GPU.
    if hasattr(self, 'getDevice'):
        target_device = idx if idx else torch._C._cuda_getDevice()
        if self.getDevice() != target_device:
            with device(target_device):
                return type(self)(self.size()).copy_(self)
    else:
        ctx = device(idx) if idx else _dummy_ctx()
        with ctx:
            return self.type(getattr(torch.cuda, self.__class__.__name__))
_TensorBase.cuda = _tensor_cuda

def _tensor_cpu(self):
    return self.type(getattr(torch, self.__class__.__name__))
_TensorBase.cpu = _tensor_cpu

def deviceCount():
    return torch._C._cuda_getDeviceCount()

assert torch._C._cuda_init()
