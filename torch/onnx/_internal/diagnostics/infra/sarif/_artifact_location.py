# DO NOT EDIT! This file was generated by jschema_to_python version 0.0.1.dev29,
# with extension for dataclasses and type annotation.

from __future__ import annotations

import dataclasses
from typing import Optional

from torch.onnx._internal.diagnostics.infra.sarif import _message, _property_bag


@dataclasses.dataclass
class ArtifactLocation:
    """Specifies the location of an artifact."""

    description: _message.Message | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "description"}
    )
    index: int = dataclasses.field(
        default=-1, metadata={"schema_property_name": "index"}
    )
    properties: _property_bag.PropertyBag | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "properties"}
    )
    uri: str | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "uri"}
    )
    uri_base_id: str | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "uriBaseId"}
    )


# flake8: noqa
