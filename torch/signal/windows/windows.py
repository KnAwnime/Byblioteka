import warnings

import torch
import numpy as np

from torch import Tensor
from torch.types import _dtype, _device, _layout

__all__ = [
    'cosine',
    'exponential',
    'gaussian',
]

_eps = 1e-10  # Used to fix floating point errors


def _window_function_checks(function_name: str, window_length: int, dtype: _dtype, layout: _layout) -> None:
    r"""Performs common checks for all the defined windows.
     This function should be called before computing any window

     Args:
         function_name (str): name of the window function.
         window_length (int): length of the window.
         dtype (:class:`torch.dtype`): the desired data type of the window tensor.
         layout (:class:`torch.layout`): the desired layout of the window tensor.
     """

    def is_floating_type(t: _dtype) -> bool:
        return t == torch.float32 or t == torch.bfloat16 or t == torch.float64 or t == torch.float16

    def is_complex_type(t: _dtype) -> bool:
        return t == torch.complex64 or t == torch.complex128 or t == torch.complex32

    if window_length < 0:
        raise RuntimeError(f'{function_name} requires non-negative window_length, got window_length={window_length}')
    if layout is not torch.strided:
        raise RuntimeError(f'{function_name} is not implemented for sparse types, got: {layout}')
    if not is_floating_type(dtype) and not is_complex_type(dtype):
        raise RuntimeError(f'{function_name} expects floating point dtypes, got: {dtype}')


def _window_length_check(desired_length, output_length):
    r"""Performs window length check.
     This function should be called after computing windows with `torch.arange`
     if the step is floating point.

     Args:
         desired_length (int): desired length of the window.
         output_length (int): output length of the window.
     """
    if desired_length != output_length:
        warnings.warn(('The difference in length is subject to floating points rounding errors.'
                       f'Expected length: {output_length}. Output length: {desired_length}'))


def exponential(window_length: int,
                periodic: bool = True,
                center: float = None,
                tau: float = 1.0,
                dtype: _dtype = None,
                layout: _layout = torch.strided,
                device: _device = None,
                requires_grad: bool = False) -> Tensor:
    if dtype is None:
        dtype = torch.get_default_dtype()

    _window_function_checks('exponential', window_length, dtype, layout)

    if window_length == 0:
        return torch.empty((0,), dtype=dtype, layout=layout, device=device, requires_grad=requires_grad)

    if window_length == 1:
        return torch.ones((1,), dtype=dtype, layout=layout, device=device, requires_grad=requires_grad)

    if periodic and center is not None:
        raise ValueError('Center must be \'None\' for periodic equal True')

    if tau <= 0:
        raise ValueError(f'Tau cannot must be positive, got: {tau} instead.')

    if center is None:
        center = -(window_length if periodic else window_length - 1) / 2.0

    constant = 1 / tau
    k = torch.arange(center * constant,
                     (center + (window_length - 1)) * constant + _eps,
                     constant,
                     dtype=dtype,
                     layout=layout,
                     device=device,
                     requires_grad=requires_grad)

    _window_length_check(window_length, k.size()[0])

    return torch.exp(-torch.abs(k))


def cosine(window_length: int,
           periodic: bool = True,
           dtype: _dtype = None,
           layout: _layout = torch.strided,
           device: _device = None,
           requires_grad: bool = False) -> Tensor:
    if dtype is None:
        dtype = torch.get_default_dtype()

    _window_function_checks('cosine', window_length, dtype, layout)

    if window_length == 0:
        return torch.empty((0,), dtype=dtype, layout=layout, device=device, requires_grad=requires_grad)

    if window_length == 1:
        return torch.ones((1,), dtype=dtype, layout=layout, device=device, requires_grad=requires_grad)

    start = 0.5
    constant = torch.pi / (window_length + 1 if periodic else window_length)

    """
    Note that non-integer step is subject to floating point rounding errors when comparing against end; 
    to avoid inconsistency, we advise adding a small epsilon to end in such cases.
    """
    k = torch.arange(start * constant,
                     (start + (window_length - 1)) * constant + _eps,
                     step=constant,
                     dtype=dtype,
                     layout=layout,
                     device=device,
                     requires_grad=requires_grad)

    return torch.sin(k)


def gaussian(window_length: int,
             periodic: bool = True,
             std: float = 1.,
             dtype: _dtype = None,
             layout: _layout = torch.strided,
             device: _device = None,
             requires_grad: bool = False) -> Tensor:
    if dtype is None:
        dtype = torch.get_default_dtype()

    _window_function_checks('gaussian', window_length, dtype, layout)

    if window_length == 0:
        return torch.empty((0,), dtype=dtype, layout=layout, device=device, requires_grad=requires_grad)

    if window_length == 1:
        return torch.ones((1,), dtype=dtype, layout=layout, device=device, requires_grad=requires_grad)

    start = -(window_length if periodic else window_length - 1) / 2.0

    constant = 1 / (std * np.sqrt(2))

    """
    Note that non-integer step is subject to floating point rounding errors when comparing against end; 
    to avoid inconsistency, we advise adding a small epsilon to end in such cases.
    """
    k = torch.arange(start * constant,
                     (start + (window_length - 1)) * constant + _eps,
                     step=constant,
                     dtype=dtype,
                     layout=layout,
                     device=device,
                     requires_grad=requires_grad)

    _window_length_check(window_length, k.size()[0])

    return torch.exp(-k ** 2)
