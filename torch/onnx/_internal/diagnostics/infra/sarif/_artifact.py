# DO NOT EDIT! This file was generated by jschema_to_python version 0.0.1.dev29,
# with extension for dataclasses and type annotation.

from __future__ import annotations

import dataclasses
from typing import Any, List, Literal, Optional

from torch.onnx._internal.diagnostics.infra.sarif import (
    _artifact_content,
    _artifact_location,
    _message,
    _property_bag,
)


@dataclasses.dataclass
class Artifact:
    """A single artifact. In some cases, this artifact might be nested within another artifact."""

    contents: _artifact_content.ArtifactContent | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "contents"}
    )
    description: _message.Message | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "description"}
    )
    encoding: str | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "encoding"}
    )
    hashes: Any = dataclasses.field(
        default=None, metadata={"schema_property_name": "hashes"}
    )
    last_modified_time_utc: str | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "lastModifiedTimeUtc"}
    )
    length: int = dataclasses.field(
        default=-1, metadata={"schema_property_name": "length"}
    )
    location: _artifact_location.ArtifactLocation | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "location"}
    )
    mime_type: str | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "mimeType"}
    )
    offset: int | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "offset"}
    )
    parent_index: int = dataclasses.field(
        default=-1, metadata={"schema_property_name": "parentIndex"}
    )
    properties: _property_bag.PropertyBag | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "properties"}
    )
    roles: None | (
        list[
            Literal[
                "analysisTarget",
                "attachment",
                "responseFile",
                "resultFile",
                "standardStream",
                "tracedFile",
                "unmodified",
                "modified",
                "added",
                "deleted",
                "renamed",
                "uncontrolled",
                "driver",
                "extension",
                "translation",
                "taxonomy",
                "policy",
                "referencedOnCommandLine",
                "memoryContents",
                "directory",
                "userSpecifiedConfiguration",
                "toolSpecifiedConfiguration",
                "debugOutputFile",
            ]
        ]
    ) = dataclasses.field(default=None, metadata={"schema_property_name": "roles"})
    source_language: str | None = dataclasses.field(
        default=None, metadata={"schema_property_name": "sourceLanguage"}
    )


# flake8: noqa
