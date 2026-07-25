"""Probe source files for API surface, comments, and short excerpts."""
from __future__ import annotations

import re
from pathlib import Path

from .paths import ROOT

_CODE_SUFFIX = {
    ".hpp", ".cpp", ".h", ".c", ".glsl", ".vert", ".frag", ".comp",
    ".rgen", ".rmiss", ".rchit", ".rahit",
}


def extract_header_notes(files: list[str], max_paras: int = 8) -> list[str]:
    notes: list[str] = []
    for f in files:
        p = ROOT / f
        if not p.is_file() or p.suffix not in _CODE_SUFFIX:
            continue
        try:
            text = p.read_text(errors="replace")
        except OSError:
            continue
        lines = text.splitlines()[:160]
        block: list[str] = []
        in_block = False
        for ln in lines:
            s = ln.strip()
            if s.startswith("/*") or s.startswith("/**"):
                in_block = True
                s = s.lstrip("/*").lstrip("*").strip()
                if s.endswith("*/"):
                    s = s[:-2].strip()
                    in_block = False
                if s:
                    block.append(s)
                continue
            if in_block:
                if "*/" in s:
                    s = s.split("*/")[0].strip().lstrip("*").strip()
                    in_block = False
                else:
                    s = s.lstrip("*").strip()
                if s and not s.startswith("@"):
                    block.append(s)
                continue
            if s.startswith("//"):
                c = s[2:].strip()
                if c and not c.startswith("---") and "Copyright" not in c:
                    block.append(c)
                continue
            if s.startswith(("#", "layout", "#include", "#version")):
                continue
            if s.startswith(("namespace", "class ", "struct ", "enum ")):
                break
            if s and not s.startswith(("using ", "template")) and not s.startswith("{"):
                break
        for ln in lines:
            m = re.match(r"^\s*(class|struct|enum class)\s+(\w+)", ln)
            if m and m.group(2) not in ("Ptr", "Event"):
                notes.append(f"{p.name}: defines `{m.group(1)} {m.group(2)}`.")
                if len(notes) >= 3:
                    break
        joined = re.sub(r"\s+", " ", " ".join(block)).strip()
        if len(joined) > 40:
            for part in re.split(r"(?<=[.!?])\s+", joined):
                part = part.strip()
                if len(part) > 30:
                    notes.append(part[:480] + ("…" if len(part) > 480 else ""))
                if len(notes) >= max_paras:
                    break
        if len(notes) >= max_paras:
            break
    seen: set[str] = set()
    out: list[str] = []
    for n in notes:
        k = n[:80]
        if k not in seen:
            seen.add(k)
            out.append(n)
    return out[:max_paras]


def extract_api(files: list[str], max_items: int = 18) -> list[str]:
    """Public methods / GLSL functions / layout bindings."""
    items: list[str] = []
    for f in files:
        p = ROOT / f
        if not p.is_file() or p.suffix not in _CODE_SUFFIX:
            continue
        try:
            text = p.read_text(errors="replace")
        except OSError:
            continue
        # C++ methods / free functions
        for m in re.finditer(
            r"^\s*(?:\[\[nodiscard\]\]\s*)?(?:virtual\s+)?(?:static\s+)?"
            r"(?:inline\s+)?([\w:<>*&,\s]+?)\s+(\w+)\s*\([^;]*\)\s*(?:const)?\s*(?:noexcept)?\s*[;{]",
            text,
            re.M,
        ):
            ret, name = m.group(1).strip(), m.group(2)
            if name in ("if", "for", "while", "switch", "return", "sizeof") or name.startswith("_"):
                continue
            if len(ret) > 60:
                continue
            items.append(f"`{name}()` — {Path(f).name}")
            if len(items) >= max_items:
                return items
        # GLSL functions (skip ubiquitous builtins noise)
        _skip = {
            "main", "normalize", "mix", "clamp", "dot", "cross", "length", "abs",
            "min", "max", "pow", "exp", "log", "sin", "cos", "vec2", "vec3", "vec4",
            "mat3", "mat4", "imageStore", "imageLoad", "texture", "texelFetch",
        }
        for m in re.finditer(r"^\s*(?:void|float|vec[234]|mat[234]|uint|int|bool)\s+(\w+)\s*\(", text, re.M):
            name = m.group(1)
            if name in _skip:
                continue
            items.append(f"`{name}()` — {Path(f).name}")
            if len(items) >= max_items:
                return items
        # layout bindings
        for m in re.finditer(r"layout\s*\([^)]*binding\s*=\s*(\d+)[^)]*\)\s*([^;]+);", text):
            items.append(f"binding {m.group(1)}: `{m.group(2).strip()[:60]}` — {Path(f).name}")
            if len(items) >= max_items:
                return items
    # de-dupe
    seen: set[str] = set()
    out = []
    for i in items:
        if i not in seen:
            seen.add(i)
            out.append(i)
    return out[:max_items]


def extract_snippet(files: list[str], max_lines: int = 24) -> tuple[str, str] | None:
    """Return (path, code) for a representative snippet."""
    for f in files:
        p = ROOT / f
        if not p.is_file() or p.suffix not in _CODE_SUFFIX:
            continue
        try:
            lines = p.read_text(errors="replace").splitlines()
        except OSError:
            continue
        # Prefer a meaty region after includes
        start = 0
        for i, ln in enumerate(lines[:80]):
            if ln.startswith("#include") or ln.startswith("#version") or ln.startswith("#extension"):
                start = i + 1
        # skip blank
        while start < len(lines) and not lines[start].strip():
            start += 1
        chunk = lines[start : start + max_lines]
        if len("\n".join(chunk).strip()) < 40:
            continue
        return f, "\n".join(chunk)
    return None


def probe(files: list[str]) -> dict:
    return {
        "notes": extract_header_notes(files),
        "api": extract_api(files),
        "snippet": extract_snippet(files),
    }
