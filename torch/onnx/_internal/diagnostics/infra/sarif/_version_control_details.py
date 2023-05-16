# DO NOT EDIT! This file was generated by jschema_to_python version 0.0.1.dev29,
# with extension for dataclasses and type annotation.

from __future__ import annotations

import dataclasses
from typing import Optional

from torch.onnx._internal.diagnostics.infra.sarif import (
    _artifact_location,
    _property_bag,
)


@dataclasses.dataclass
class VersionControlDetails:
    """Specifies the information necessary to retrieve a desired revision from a version control system."""

    repository_uri: str = dataclasses.field(
        metadata={"schema_property_name": "repositoryUri"}
    )
    as_of_time_utc: str | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "asOfTimeUtc"}
    )
    branch: str | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "branch"}
    )
    mapped_to: _artifact_location.ArtifactLocation | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "mappedTo"}
    )
    properties: _property_bag.PropertyBag | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "properties"}
    )
    revision_id: str | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "revisionId"}
    )
    revision_tag: str | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "revisionTag"}
    )


# flake8: noqa
