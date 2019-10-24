from __future__ import absolute_import, division, print_function, unicode_literals

import torch


def is_available():
    return (hasattr(torch._C, "_c10d_init") and hasattr(torch._C, "_rpc_init")
            and hasattr(torch._C, "_dist_autograd_init"))


if is_available() and not (torch._C._c10d_init() and torch._C._rpc_init() and torch._C._dist_autograd_init()):
    raise RuntimeError("Failed to initialize PyTorch distributed support")


if is_available():
    from .distributed_c10d import *
    # Variables prefixed with underscore are not auto imported
    # See the comment in `distributed_c10d.py` above `_backend` on why we expose
    # this.

    from .distributed_c10d import _backend
