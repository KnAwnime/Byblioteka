# mypy: ignore-errors

import inspect
from typing import Dict, List

import torch
from .. import variables
from ..exc import unimplemented
from ..utils import istype
from .base import VariableTracker
from .constant import ConstantVariable


class DistributedVariable(VariableTracker):
    """
    The base distributed variable that encapsulates common methods
    for the distributed objects (i.e. ProcessGroup, DeviceMesh, etc.).
    Concrete distributed objects could inherit this class and add object
    specific logic.

    i.e. It provides the check on the distributed package existance
    and hold the tracking value for the corresponding distributed object.
    """

    def __init__(self, value, **kwargs):
        super().__init__(**kwargs)
        if not DistributedVariable.is_available():
            unimplemented("torch.distributed package is not available!")
        self.value = value

    def python_type(self):
        return type(self.value)

    @staticmethod
    def is_available():
        # check if the distributed package is available or not
        return torch.distributed.is_available()


def is_from_local(value):
    if not DistributedVariable.is_available():
        return False
    from torch.distributed._tensor import DTensor

    return inspect.isfunction(value) and value is DTensor.from_local


def is_constant_pg_functions(value):
    if not DistributedVariable.is_available():
        return False

    from torch.distributed.distributed_c10d import (
        _get_group_size_by_name,
        _get_group_tag,
        _rank_not_in_group,
        _resolve_group_name_by_ranks_and_tag,
        get_process_group_ranks,
    )

    constant_processgroup_functions = [
        _get_group_size_by_name,
        _get_group_tag,
        _rank_not_in_group,
        get_process_group_ranks,
        _resolve_group_name_by_ranks_and_tag,
    ]

    return inspect.isfunction(value) and value in constant_processgroup_functions


class PlacementClassVariable(DistributedVariable):
    @staticmethod
    def is_placement_type(value):
        # we can't rely on importing/accessing torch distributed, it is not always built.
        if not DistributedVariable.is_available():
            return False

        from torch.distributed._tensor.placement_types import Placement

        return type(value) is type and issubclass(value, Placement)

    def call_function(
        self, tx, args: "List[VariableTracker]", kwargs: "Dict[str, VariableTracker]"
    ) -> "VariableTracker":
        if (
            inspect.getattr_static(self.value, "__new__", None) in (object.__new__,)
            and self.source
        ):
            # NOTE: we don't need to track mutations to the placement class as they
            # suppose to be immutable.
            new_obj = object.__new__(self.value)
            var = PlacementVariable(new_obj)
            if inspect.getattr_static(self.value, "__init__", None):
                var.call_method(tx, "__init__", args, kwargs)
                return var

        return super().call_function(tx, args, kwargs)


class PlacementVariable(DistributedVariable):
    @staticmethod
    def is_placement(value):
        # we can't rely on importing/accessing torch distributed, it is not always built.
        if not DistributedVariable.is_available():
            return False

        from torch.distributed._tensor.placement_types import Placement

        return isinstance(value, Placement)

    def as_python_constant(self):
        return self.value

    def var_getattr(self, tx, name: str) -> VariableTracker:
        if name == "dim":
            return ConstantVariable.create(self.value.dim)
        return super().var_getattr(tx, name)

    def call_method(
        self,
        tx,
        name,
        args: "List[VariableTracker]",
        kwargs: "Dict[str, VariableTracker]",
    ) -> "VariableTracker":
        from . import ConstantVariable

        # Placement types dynamo tracking only allows following methods
        # and __setattr__  is for case like `Shard(dim)` and methods.
        # Methods in the list must satisfy:
        #    1. Input arguments are constants and do not need to be guarded on;
        #    2. Output is constant with respect to their inputs
        constant_fold_functions = [
            "__init__",
            "__setattr__",
            "is_shard",
            "is_partial",
            "is_replicate",
        ]

        if name in constant_fold_functions:
            try:
                value_type = type(self.value)
                assert (
                    inspect.getattr_static(value_type, "__getattr__", None) is None
                ), "no custom getattr allowed!"
                method = inspect.getattr_static(value_type, name)
            except AttributeError:
                method = None
            if method is object.__init__:
                return ConstantVariable.create(None)

            args = [x.as_python_constant() for x in args]
            kwargs = {k: v.as_python_constant() for k, v in kwargs.items()}
            if name == "__setattr__":
                method(self.value, *args, **kwargs)
                return self
            constant_val = method(self.value, *args, **kwargs)
            return ConstantVariable.create(constant_val)

        return super().call_method(tx, name, args, kwargs)


class DeviceMeshVariable(DistributedVariable):
    @staticmethod
    def is_device_mesh(value):
        # we can't rely on importing/accessing torch distributed, it is not always built.
        if not DistributedVariable.is_available():
            return False

        from torch.distributed.device_mesh import DeviceMesh

        return istype(value, DeviceMesh)

    def as_python_constant(self):
        return self.value

    def var_getattr(self, tx, name: str) -> VariableTracker:
        if name == "ndim":
            return ConstantVariable.create(self.value.ndim)
        return super().var_getattr(tx, name)

    def call_method(
        self,
        tx,
        name,
        args: "List[VariableTracker]",
        kwargs: "Dict[str, VariableTracker]",
    ) -> "VariableTracker":
        if name == "size":
            const_args = [x.as_python_constant() for x in args]
            const_kwargs = {k: v.as_python_constant() for k, v in kwargs.items()}
            return ConstantVariable.create(self.value.size(*const_args, **const_kwargs))
        if name == "get_coordinate":
            return ConstantVariable.create(self.value.get_coordinate())
        if name == "get_group":
            return ConstantVariable.create(self.value.get_group())
        if name == "_get_or_create_default_group":
            return ProcessGroupVariable(self.value._get_or_create_default_group())
        return super().call_method(tx, name, args, kwargs)


class ProcessGroupVariable(DistributedVariable):
    """
    We don't want a ProcessGroup object to end up in our output graph.

    But it's common for dynamo to intercept a PG that is then used to get info like
    rank() or world_size(), as well as passed to utility functions in distributed_c10d
    which desugar it into plain types like a ranklist and tag.

    For convenience and proper guarding, we construct a variable type.

    TODO: make it possible to use ProcessGroupVariable as input to simple functions
          like _expand_group without dynamo complaining about making a proxy for it.
          It is not a tensor-like type, and we don't want a proxy- but dynamo assumes
          torch library functions are dealing with tensor-like types and would have proxies
          for their args.
    TODO: should we make this inherit VT instead of UDOV? Do we want any of the default behaviors
          or just graph-break whenever one of our special cases is not hit?
    """

    def as_python_constant(self):
        return self.value

    def call_method(
        self,
        tx,
        name,
        args: "List[VariableTracker]",
        kwargs: "Dict[str, VariableTracker]",
    ) -> "VariableTracker":
        if name == "rank":
            return variables.ConstantVariable.create(self.value.rank())
        if name == "size":
            return variables.ConstantVariable.create(self.value.size())

        return super().call_method(tx, name, args, kwargs)

    def var_getattr(self, tx, name):
        if name == "group_name":
            return variables.ConstantVariable.create(self.value.group_name)
        if name in ["rank", "size"]:
            return variables.LambdaVariable(
                lambda *args, **kwargs: self.call_method(tx, name, args, kwargs)
            )
        # TODO should this just raise unimplemented?
        return super().var_getattr(tx, name)

    @staticmethod
    def is_process_group(value):
        # we can't rely on importing/accessing torch distributed, it is not always built.
        if not DistributedVariable.is_available():
            return False
        from torch._C._distributed_c10d import ProcessGroup
        from torch.testing._internal.distributed.fake_pg import FakeProcessGroup

        return istype(value, (ProcessGroup, FakeProcessGroup))
