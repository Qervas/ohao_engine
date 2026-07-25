from __future__ import annotations
from dataclasses import dataclass, field
from typing import Any


def page(
    id_: str,
    title: str,
    summary: str,
    files: list[str] | None = None,
    topics: list[str] | None = None,
    design: list[str] | None = None,
    workflow: list[str] | None = None,
    why: str | None = None,
    what: list[str] | None = None,
    how: list[str] | None = None,
    contracts: list[str] | None = None,
    math: list[str] | None = None,
    diagram: str | None = None,  # svg filename under assets/diagrams or inline svg id
) -> dict[str, Any]:
    """Design-unit node in the monograph tree."""
    return {
        "id": id_,
        "title": title,
        "summary": summary,
        "files": files or [],
        "topics": topics or [],
        "design": design or [],
        "workflow": workflow or [],
        "why": why,
        "what": what or [],
        "how": how or [],
        "contracts": contracts or [],
        "math": math or [],
        "diagram": diagram,
    }


@dataclass
class Unit:
    """Optional typed view of a page() dict."""
    id: str
    title: str
    summary: str
    files: list[str] = field(default_factory=list)
    topics: list[str] = field(default_factory=list)
    design: list[str] = field(default_factory=list)
    workflow: list[str] = field(default_factory=list)
    why: str | None = None
    what: list[str] = field(default_factory=list)
    how: list[str] = field(default_factory=list)
    contracts: list[str] = field(default_factory=list)
    math: list[str] = field(default_factory=list)
    diagram: str | None = None

    @classmethod
    def from_dict(cls, d: dict) -> "Unit":
        return cls(**{k: d.get(k, getattr(cls, k, None) if False else d.get(k)) for k in cls.__dataclass_fields__})
