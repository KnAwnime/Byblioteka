# DO NOT EDIT! This file was generated by jschema_to_python version 0.0.1.dev29,
# with extension for dataclasses and type annotation.

from __future__ import annotations

import dataclasses
from typing import Optional

from torch.onnx._internal.diagnostics.infra.sarif import _property_bag


@dataclasses.dataclass
class LogicalLocation:
    """A logical location of a construct that produced a result."""

    decorated_name: str | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "decoratedName"}
    )
    fully_qualified_name: str | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "fullyQualifiedName"}
    )
    index: int = dataclasses.field(
        default=-1, metadata={"schema_property_name": "index"}
    )
    kind: str | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "kind"}
    )
    name: str | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "name"}
    )
    parent_index: int = dataclasses.field(
        default=-1, metadata={"schema_property_name": "parentIndex"}
    )
    properties: _property_bag.PropertyBag | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "properties"}
    )


# flake8: noqa
