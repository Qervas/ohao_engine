"""Lightweight SVG diagrams for design units (no external tools)."""
from __future__ import annotations

from .htmlutil import esc
from .paths import DIAGRAMS


def workflow_svg(title: str, steps: list[str], out_name: str | None = None) -> str:
    """Horizontal/vertical flow of steps → SVG markup (and optional file)."""
    if not steps:
        return ""
    # Vertical flow, textbook ink palette
    w = 720
    row_h = 52
    pad = 28
    h = pad * 2 + len(steps) * row_h + 20
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" '
        f'class="unit-diagram" role="img" aria-label="{esc(title)}">',
        f'<rect width="100%" height="100%" fill="#0f1419"/>',
        f'<text x="{pad}" y="22" fill="#c4a574" font-family="IBM Plex Mono,monospace" '
        f'font-size="11" letter-spacing="1.5">{esc(title.upper())}</text>',
    ]
    for i, step in enumerate(steps):
        y = pad + 18 + i * row_h
        # box
        parts.append(
            f'<rect x="{pad}" y="{y}" width="{w - pad * 2}" height="40" rx="3" '
            f'fill="#1a222c" stroke="#3d4a58" stroke-width="1"/>'
        )
        parts.append(
            f'<text x="{pad + 14}" y="{y + 25}" fill="#e8e2d6" '
            f'font-family="Source Serif 4,Georgia,serif" font-size="14">'
            f'<tspan fill="#c4784a" font-family="IBM Plex Mono,monospace" font-size="12">'
            f"{i + 1:02d}</tspan>"
            f'  {esc(step[:72])}</text>'
        )
        if i < len(steps) - 1:
            cy = y + 40
            parts.append(
                f'<line x1="{w // 2}" y1="{cy}" x2="{w // 2}" y2="{cy + 12}" '
                f'stroke="#c4784a" stroke-width="1.5"/>'
            )
    parts.append("</svg>")
    svg = "\n".join(parts)
    if out_name:
        DIAGRAMS.mkdir(parents=True, exist_ok=True)
        (DIAGRAMS / out_name).write_text(svg)
    return svg
