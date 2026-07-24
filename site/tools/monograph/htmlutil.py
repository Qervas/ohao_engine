from __future__ import annotations

import html


def esc(s: str) -> str:
    return html.escape(s, quote=True)


def paras(lines: list[str], cls: str = "") -> str:
    if not lines:
        return ""
    attr = f' class="{cls}"' if cls else ""
    return "\n".join(f"<p{attr}>{esc(p)}</p>" for p in lines if p)


def bullets(items: list[str]) -> str:
    if not items:
        return ""
    lis = "\n".join(f"<li>{esc(t)}</li>" for t in items)
    return f"<ul>\n{lis}\n</ul>"


def algo(title: str, steps: list[str]) -> str:
    if not steps:
        return ""
    lis = "\n".join(f"<li>{esc(s)}</li>" for s in steps)
    return f'''
      <div class="algo reveal"><div class="algo-bar">{esc(title)}</div>
        <ol>\n{lis}\n        </ol></div>
'''
