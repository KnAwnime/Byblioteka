import operator
from typing import Dict, List

import torch
from torch._dynamo.source import GetItemSource

from .. import variables
from ..exc import unimplemented, UserError, UserErrorType
from ..guards import GuardBuilder
from ..utils import np
from .base import typestr, VariableTracker

_type_to_assert_reason = {
    # NB - We CAN have ConstantVariable.create(set) because of how sets interact with guards.
    # A locally created set should always become a SetVariable, as the items in the set will already either be sourced
    # from somewhere else, or unsourced. An input set would imply sources derived from set contents. For example, an
    # input list's contents will have a source like some_list[0], some_list[1][1], etc. For a set, arbitrary access is
    # not possible. This is a solvable problem, but one we have not taken on yet. As such, input sets are not allowed to
    # become SetVariables. The solution here is to create a ConstantSetVariable that is more like a ConstantVariable.
    # As this does not exist, we cannot add sets to this invariant.
    list: "List types must use ListVariable.",
    dict: "Dict types must use ConstDictVariable.",
    torch.Tensor: "Tensor types must use TensorVariable.",
    torch.SymInt: "SymInts must use SymNodeVariable. "
    "If the underlying value is static, we will create a ConstantVariable and specialize.",
    torch.SymFloat: "SymInts must use SymNodeVariable",
}


class ConstantVariable(VariableTracker):
    @staticmethod
    def create(value, **kwargs):
        source = kwargs.get("source", None)
        is_literal = ConstantVariable.is_literal(value)
        if not is_literal:
            for disallowed_type, reason in _type_to_assert_reason.items():
                assert not isinstance(value, disallowed_type), reason

        # Routing for list and tuple literals.
        if is_literal and isinstance(value, (list, tuple)):
            items = []
            for i, x in enumerate(value):
                item_source = GetItemSource(source, i) if source else None
                guards = (
                    {item_source.make_guard(GuardBuilder.CONSTANT_MATCH)}
                    if item_source
                    else None
                )
                items.append(
                    ConstantVariable.create(
                        x,
                        source=item_source,
                        guards=guards,
                    )
                )
            return variables.BaseListVariable.cls_for(type(value))(
                items, regen_guards=True, **kwargs
            )

        return ConstantVariable(value, **kwargs)

    def __init__(self, value, **kwargs):
        super().__init__(**kwargs)
        if not ConstantVariable.is_literal(value):
            for disallowed_type, reason in _type_to_assert_reason.items():
                assert not isinstance(value, disallowed_type), reason

        assert not isinstance(
            value, (list, tuple)
        ), "ConstantVariable(list) is banned - please create a ListVariable(items)"
        if np is not None and isinstance(value, np.number):
            self.value = value.item()
        else:
            self.value = value

    def as_proxy(self):
        return self.value

    def __str__(self):
        # return f"ConstantVariable({self.value})"
        return f"ConstantVariable({type(self.value).__name__})"

    def python_type(self):
        return type(self.value)

    def as_python_constant(self):
        return self.value

    @property
    def items(self):
        """
        Need this when adding a BaseListVariable and a ConstantVariable together.
        Happens in detectron2.
        """
        return self.unpack_var_sequence(tx=None)

    def getitem_const(self, arg: VariableTracker):
        return ConstantVariable.create(
            self.value[arg.as_python_constant()],
            **VariableTracker.propagate([self, arg]),
        )

    @staticmethod
    def is_literal(obj):
        if type(obj) in (int, float, bool, type(None), str, Ellipsis.__class__):
            return True
        if type(obj) in (list, tuple, set, frozenset):
            return all(ConstantVariable.is_literal(x) for x in obj)
        return False

    def unpack_var_sequence(self, tx):
        try:
            options = VariableTracker.propagate([self])
            return [
                ConstantVariable.create(x, **options) for x in self.as_python_constant()
            ]
        except TypeError as e:
            raise NotImplementedError from e

    def const_getattr(self, tx, name):
        if isinstance(self.value, type):
            raise UserError(
                UserErrorType.ANTI_PATTERN,
                "Can't access members of type(obj) for a generated custom object. "
                "Please use __class__ instead",
            )
        member = getattr(self.value, name)
        if callable(member):
            raise NotImplementedError()
        return member

    def call_method(
        self,
        tx,
        name,
        args: "List[VariableTracker]",
        kwargs: "Dict[str, VariableTracker]",
    ) -> "VariableTracker":
        from .tensor import SymNodeVariable

        options = VariableTracker.propagate(self, args, kwargs.values())

        if any(isinstance(x, SymNodeVariable) for x in args):
            # Promote to SymNodeVariable for operations involving dynamic shapes.
            return variables.SymNodeVariable(self.as_proxy(), self.value).call_method(
                tx, name, args, kwargs
            )

        try:
            const_args = [a.as_python_constant() for a in args]
            const_kwargs = {k: v.as_python_constant() for k, v in kwargs.items()}
        except NotImplementedError:
            return super().call_method(tx, name, args, kwargs)

        def has_arith_binop(num_ty):
            return (
                isinstance(self.value, num_ty)
                and hasattr(operator, name)
                and len(args) == 1
                and args[0].is_python_constant()
            )

        if isinstance(self.value, str) and name in str.__dict__.keys():
            method = getattr(self.value, name)
            return ConstantVariable.create(
                method(*const_args, **const_kwargs), **options
            )
        elif has_arith_binop(int) or has_arith_binop(float):
            op = getattr(operator, name)
            add_target = const_args[0]
            if isinstance(add_target, (torch.SymInt, torch.SymFloat)):
                from .tensor import SymNodeVariable

                # Addition between a non sym and sym makes a sym
                # sym_num = tx.output.register_attr_or_module(
                #     add_target, f"sym_shape_{add_target}", source=None
                # )
                proxy = tx.output.create_proxy(
                    "call_function", op, (self.value, add_target), {}
                )
                return SymNodeVariable.create(tx, proxy, add_target, **options)
            return ConstantVariable.create(op(self.value, add_target), **options)
        elif name == "__len__" and not (args or kwargs):
            return ConstantVariable.create(len(self.value), **options)
        elif name == "__contains__" and len(args) == 1 and args[0].is_python_constant():
            assert not kwargs
            search = args[0].as_python_constant()
            result = search in self.value
            return ConstantVariable.create(result, **options)

        unimplemented(f"const method call {typestr(self.value)}.{name}")

    def call_hasattr(self, tx, name: str) -> "VariableTracker":
        result = hasattr(self.value, name)
        return variables.ConstantVariable.create(result).add_options(self)


class EnumVariable(VariableTracker):
    def __init__(self, value, **kwargs):
        super().__init__(**kwargs)
        self.value = value

    def as_proxy(self):
        return self.value

    def __str__(self):
        return f"EnumVariable({type(self.value)})"

    def python_type(self):
        return type(self.value)

    def as_python_constant(self):
        return self.value

    def const_getattr(self, tx, name):
        member = getattr(self.value, name)
        if callable(member):
            raise NotImplementedError()
        return member
