"""Load optional rich markdown for a design unit.

Path: site/content/units/<module>/<id>.md

Sections (## headers):
  What, How, Why, Contracts, Math, Notes
"""
from __future__ import annotations

import re
from pathlib import Path

from .paths import CONTENT


def load_unit_md(module: str, unit_id: str) -> dict[str, list[str] | str]:
    path = CONTENT / module / f"{unit_id}.md"
    if not path.is_file():
        return {}
    text = path.read_text(encoding="utf-8")
    # strip frontmatter
    if text.startswith("---"):
        end = text.find("\n---", 3)
        if end != -1:
            text = text[end + 4 :]
    sections: dict[str, list[str]] = {}
    current = "notes"
    sections[current] = []
    for line in text.splitlines():
        m = re.match(r"^##\s+(.+?)\s*$", line)
        if m:
            current = m.group(1).strip().lower()
            sections.setdefault(current, [])
            continue
        if line.strip():
            sections.setdefault(current, []).append(line.rstrip())
    # join paragraph lines (blank already dropped — treat consecutive as paras via empty? we dropped blanks)
    # Re-parse more carefully for paragraphs
    return _paras_from_sections(path.read_text(encoding="utf-8"))


def _paras_from_sections(raw: str) -> dict[str, list[str] | str]:
    if raw.startswith("---"):
        end = raw.find("\n---", 3)
        if end != -1:
            raw = raw[end + 4 :]
    out: dict[str, list[str] | str] = {}
    current = "notes"
    buf: list[str] = []

    def flush():
        nonlocal buf
        if not buf:
            return
        # split on blank lines into paragraphs; bullets stay list items
        para: list[str] = []
        paras: list[str] = []
        bullets: list[str] = []
        for ln in buf:
            if ln.strip().startswith(("- ", "* ")):
                if para:
                    paras.append(" ".join(para))
                    para = []
                bullets.append(ln.strip()[2:].strip())
            elif not ln.strip():
                if para:
                    paras.append(" ".join(para))
                    para = []
            else:
                if bullets and not para:
                    pass
                para.append(ln.strip())
        if para:
            paras.append(" ".join(para))
        key = current
        if bullets and not paras:
            out[key] = bullets
        elif bullets and paras:
            out[key] = paras + [f"• {b}" for b in bullets]
        else:
            out[key] = paras
        buf = []

    for line in raw.splitlines():
        m = re.match(r"^##\s+(.+?)\s*$", line)
        if m:
            flush()
            current = m.group(1).strip().lower()
            continue
        buf.append(line)
    flush()
    return out


def merge_content(child: dict, module: str) -> dict:
    """Overlay markdown content onto tree node."""
    md = load_unit_md(module, child["id"])
    if not md:
        return child
    c = {**child}
    mapping = {
        "what": "what",
        "how": "how",
        "why": "why",
        "contracts": "contracts",
        "math": "math",
        "design": "design",
        "notes": "design",
        "workflow": "workflow",
        "topics": "topics",
    }
    for mk, ck in mapping.items():
        if mk not in md:
            continue
        val = md[mk]
        if ck == "why" and isinstance(val, list):
            c["why"] = " ".join(val)
        elif ck == "how" and isinstance(val, list):
            # Split bullet-only lines into workflow; keep prose as how
            prose = [x for x in val if not str(x).startswith("• ")]
            steps = [str(x)[2:].strip() if str(x).startswith("• ") else x for x in val if str(x).startswith("• ")]
            if prose:
                c["how"] = prose
            if steps and not c.get("workflow"):
                c["workflow"] = steps
        elif isinstance(val, list):
            # skip duplicate "Source map" notes if contracts already filled
            if ck == "design" and val and str(val[0]).startswith("Source map"):
                continue
            c[ck] = val
        else:
            c[ck] = val
    return c
