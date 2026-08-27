"""Resolve {{cite path "substring"}} markers against the real repo tree.

A citation is content-anchored: the author names a unique code substring, and
the build resolves it to the current line number. Missing or ambiguous anchors
fail the build, so no citation on the site can silently go stale.

A citation may also name an explicit revision, {{cite path@rev "substring"}},
which resolves through `git show rev:path` instead of the working tree. That
is for prose about code since removed from the engine: the analysis is still
true of the revision it describes, and the rendered chip says which revision
that is rather than implying the line is current.

There is deliberately no fallback from a working-tree path to history. An
unpinned citation that goes stale must still fail the build -- that is the
guarantee this module exists to provide. Pinning is an authoring decision,
made per citation and visible in the output.
"""
from __future__ import annotations

import html
import subprocess

from .paths import ROOT

GITHUB_BLOB_BASE = "https://github.com/Qervas/ohao_engine/blob/master"


class CitationError(Exception):
    pass


def split_revision(path: str) -> tuple[str, str | None]:
    """Split "path@rev" into (path, rev). A bare path yields (path, None)."""
    if "@" not in path:
        return path, None
    file_path, _, rev = path.rpartition("@")
    if not file_path or not rev:
        raise CitationError(f"cite: malformed revision in {path!r}")
    return file_path, rev


def _read_at_revision(file_path: str, rev: str) -> str:
    try:
        out = subprocess.run(
            ["git", "show", f"{rev}:{file_path}"],
            cwd=ROOT, capture_output=True, check=True,
        )
    except FileNotFoundError as exc:
        raise CitationError("cite: git unavailable for a pinned citation") from exc
    except subprocess.CalledProcessError as exc:
        stderr = exc.stderr.decode(errors="replace").strip()
        raise CitationError(
            f"cite: {file_path!r} not found at revision {rev!r}: {stderr}"
        ) from exc
    return out.stdout.decode(errors="replace")


def resolve_citation(path: str, substring: str) -> tuple[int, str]:
    file_path, rev = split_revision(path)
    if rev is not None:
        body = _read_at_revision(file_path, rev)
    else:
        p = ROOT / file_path
        if not p.is_file():
            raise CitationError(f"cite: file not found: {file_path!r}")
        body = p.read_text(errors="replace")
    matches: list[tuple[int, str]] = []
    for i, line in enumerate(body.splitlines(), start=1):
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
    file_path, rev = split_revision(path)
    # A pinned citation shows its revision, so a reader is never led to think
    # a removed line is still in the tree.
    loc = f"{file_path}@{rev}:{line_no}" if rev else f"{file_path}:{line_no}"
    code = html.escape(text.strip(), quote=True)
    if GITHUB_BLOB_BASE:
        blob_base = (GITHUB_BLOB_BASE.rsplit("/", 1)[0] + "/" + rev) if rev else GITHUB_BLOB_BASE
        href = f"{blob_base}/{file_path}#L{line_no}"
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
