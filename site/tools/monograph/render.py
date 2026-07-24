"""HTML shell + narrative leaf rendering."""
from __future__ import annotations

from . import blocks
from .htmlutil import esc
from .paths import CONTENT, ROOT


def shell(title: str, desc: str, page_id: str, body: str, depth: int) -> str:
    rel = "../" * depth
    return f'''<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>{esc(title)} — OHAO Monograph</title>
  <meta name="description" content="{esc(desc)}" />
  <link rel="preconnect" href="https://fonts.googleapis.com" />
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin />
  <link href="https://fonts.googleapis.com/css2?family=DM+Sans:ital,opsz,wght@0,9..40,400;0,9..40,500&family=Fraunces:opsz,wght@9..144,300;9..144,500&family=IBM+Plex+Mono:wght@400;500&family=Source+Serif+4:opsz,wght@8..60,400;8..60,600&display=swap" rel="stylesheet" />
  <link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/katex.min.css" />
  <link rel="stylesheet" href="{rel}styles.css" />
</head>
<body data-page="{esc(page_id)}" data-root="{rel.rstrip("/") or "."}" data-depth="{depth}">
  <div class="progress" aria-hidden="true"></div>
  <div id="toc-mount"></div>
  <div class="mobile-toc">OHAO · Codebase tree</div>
  <main class="book">
    <article class="page">
{body}
    </article>
  </main>
  <script defer src="https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/katex.min.js"></script>
  <script defer src="https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/contrib/auto-render.min.js"></script>
  <script defer src="{rel}js/nav-tree.js"></script>
  <script defer src="{rel}js/glossary-data.js"></script>
  <script defer src="{rel}js/glossary.js"></script>
  <script defer src="{rel}js/chrome.js"></script>
</body>
</html>
'''


def _sources_footer(paths: list[str]) -> str:
    if not paths:
        return ""
    rows = "\n".join(
        f'        <div class="row"><code>{esc(p)}</code>'
        f'<span class="desc">{"" if (ROOT / p).exists() else "path note"}</span></div>'
        for p in paths
    )
    return (
        '      <div class="section-rule"><h2 id="sources">Source files</h2></div>\n'
        f'      <div class="file-map reveal">\n{rows}\n      </div>'
    )


def _on_this_page(headings: list[tuple[str, str]]) -> str:
    if not headings:
        return ""
    items = "".join(f'<li><a href="#{slug}">{esc(title)}</a></li>' for slug, title in headings)
    return (
        '      <nav class="on-this-page reveal"><div class="otp-label">On this page</div>\n'
        f'        <ol>{items}<li><a href="#sources">Sources</a></li></ol>\n'
        "      </nav>"
    )


def render_leaf(module: str, child: dict, hub: str | None) -> str:
    md_path = CONTENT / module / f'{child["id"]}.md'
    raw = md_path.read_text(encoding="utf-8") if md_path.is_file() else ""
    body_html, headings, cited = blocks.render_body(raw) if raw else ("", [], [])

    # Sources footer: citations first, then the unit's declared files, deduped.
    footer_paths: list[str] = list(cited)
    for f in child.get("files") or []:
        if f not in footer_paths:
            footer_paths.append(f)

    hub_link = f'<a href="../{hub}">← Module hub</a>' if hub else '<a href="../sitemap.html">← Sitemap</a>'
    if not body_html:
        body_html = '<div class="prose reveal"><p>Documentation for this unit is in progress.</p></div>'

    return f'''
      <p class="crumb"><a href="../../index.html">Monograph</a> ·
        <a href="../sitemap.html">Tree</a> ·
        {hub_link} · {esc(child["title"])}</p>
      <header class="chapter-head reveal">
        <div class="chapter-num">§</div>
        <div>
          <p class="chapter-kicker">{esc(module)}</p>
          <h1>{esc(child["title"])}</h1>
        </div>
      </header>
      <div class="thesis-box reveal">
        <div class="label">Design unit</div>
        <p>{esc(child["summary"])}</p>
      </div>
{_on_this_page(headings)}

{body_html}
{_sources_footer(footer_paths)}
      <div class="aside ray reveal">
        <div class="aside-label">Navigate</div>
        <p>Parent hub for the full pipeline narrative; this page is the file-level design unit.
        <a href="../sitemap.html">Sitemap</a> · hover glossary terms anywhere.</p>
      </div>
'''
