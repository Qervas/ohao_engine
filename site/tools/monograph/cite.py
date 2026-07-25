"""Resolve {{cite path "substring"}} markers against the real repo tree.

A citation is content-anchored: the author names a unique code substring, and
the build resolves it to the current line number. Missing or ambiguous anchors
fail the build, so no citation on the site can silently go stale.
"""
from __future__ import annotations

import html

from .paths import ROOT

GITHUB_BLOB_BASE = "https://github.com/Qervas/ohao_engine/blob/master"


class CitationError(Exception):
    pass


def resolve_citation(path: str, substring: str) -> tuple[int, str]:
    p = ROOT / path
    if not p.is_file():
        raise CitationError(f"cite: file not found: {path!r}")
    matches: list[tuple[int, str]] = []
    for i, line in enumerate(p.read_text(errors="replace").splitlines(), start=1):
        if substring in line:
            matches.append((i, line))
    if not matches:
        raise CitationError(
            f"cite: substring not found in {path!r}: {substring!r}"
        )
    if len(matches) > 1:
        lines = ", ".join(str(n) for n, _ in matches)
        raise CitationError(
            f"cite: ambiguous substring in {path!r} (lines {lines}): "
            f"{substring!r} — extend the substring to make it unique"
        )
    (line_no, text), = matches
    return line_no, text


def citation_chip_html(path: str, substring: str) -> str:
    line_no, text = resolve_citation(path, substring)
    loc = f"{path}:{line_no}"
    code = html.escape(text.strip(), quote=True)
    if GITHUB_BLOB_BASE:
        href = f"{GITHUB_BLOB_BASE}/{path}#L{line_no}"
        loc_html = (
            f'<a class="citation-loc" href="{href}" '
            f'target="_blank" rel="noopener">{html.escape(loc)}</a>'
        )
    else:
        loc_html = f'<span class="citation-loc">{html.escape(loc)}</span>'
    return (
        f'<div class="citation-chip">'
        f'<code class="citation-code">{code}</code>'
        f'{loc_html}'
        f'</div>'
    )
