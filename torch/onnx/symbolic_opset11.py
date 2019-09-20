import torch
import torch.onnx.symbolic_helper as sym_help

from torch.onnx.symbolic_helper import parse_args, _unimplemented
from torch.onnx.symbolic_helper import _black_list_in_opset


# EDITING THIS FILE? READ THIS FIRST!
# see Note [Edit Symbolic Files] in symbolic_helper.py

# This file exports ONNX ops for opset 11

black_listed_operators = [
    "eq", "ne", "scatter", "hardtanh"
]


for black_listed_op in black_listed_operators:
    vars()[black_listed_op] = _black_list_in_opset(black_listed_op)


def clamp(g, self, min, max):
    dtype = self.type().scalarType()

    def _cast_if_not_none(tensor, dtype):
        if tensor is not None and not tensor.node().mustBeNone():
            return g.op("Cast", tensor, to_i=sym_help.cast_pytorch_to_onnx[dtype])
        else:
            return tensor

    if dtype is not None:
        min = _cast_if_not_none(min, dtype)
        max = _cast_if_not_none(max, dtype)
    return g.op("Clip", self, min, max)


@parse_args('v', 'i')
def pixel_shuffle(g, self, upscale_factor):
    dims = self.type().sizes()
    if len(dims) != 4:
        return _unimplemented("pixel_shuffle", "only support 4d input")
    return g.op("DepthToSpace", self, blocksize_i=upscale_factor, mode_s="CRD")


@parse_args('v', 'i', 'none')
def cumsum(g, self, dim, dtype=None):
    dim_tensor = g.op("Constant", value_t=torch.tensor(dim))
    csum = g.op("CumSum", self, dim_tensor)
    if dtype and dtype.node().kind() != 'prim::Constant':
        parsed_dtype = sym_help._get_const(dtype, 'i', 'dtype')
        csum = g.op("Cast", csum, to_i=sym_help.scalar_type_to_onnx[parsed_dtype])
    return csum


@parse_args('v', 'i', 'i', 'i')
def _unique2(g, self, sorted, return_inverse, return_counts):
    u, indices, inverse_indices, counts = g.op("Unique", self, sorted_i=sorted, outputs=4)
    return u, inverse_indices, counts


@parse_args('v', 'i', 'i', 'i', 'i')
def unique_dim(g, self, dim, sorted, return_inverse, return_counts):
    u, indices, inverse_indices, counts = g.op("Unique", self, axis_i=dim, sorted_i=sorted, outputs=4)
    return u, inverse_indices, counts


@parse_args('v', 'v', 'i', 'i', 'i', 'none')
def topk(g, self, k, dim, largest, sorted, out=None):
    if out is not None:
        _unimplemented("TopK", "Out parameter is not supported for topk")
    k = sym_help._maybe_get_const(k, 'i')
    if not sym_help._is_value(k):
        k = g.op("Constant", value_t=torch.tensor(k, dtype=torch.int64))
    from torch.onnx.symbolic_opset9 import unsqueeze
    k = unsqueeze(g, k, 0)
    return g.op("TopK", self, k, axis_i=dim, largest_i=largest, sorted_i=sorted, outputs=2)

@parse_args('v', 'i', 'i', 'none')
def sort(g, self, dim, decending, out=None):
    if out is not None:
        _unimplemented("Sort", "Out parameter is not supported for sort")
    shape_ = g.op("Shape", self)
    axis = g.op("Constant", value_t=torch.tensor(0, dtype=torch.int64))
    start = g.op("Constant", value_t=torch.tensor(dim, dtype=torch.int64))
    end = g.op("Constant", value_t=torch.tensor(dim + 1, dtype=torch.int64))
    slice_ = sym_help._slice_helper(g, shape_, axes=axis, starts=start, ends=end, steps=None, dynamic_slice=True)
    return g.op("TopK", self, slice_, axis_i=dim, largest_i=decending, outputs=2)

def round(g, self):
    return g.op("Round", self)
