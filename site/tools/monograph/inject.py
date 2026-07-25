"""Hub subtree card inject + module index pages."""
from __future__ import annotations

import re

from .htmlutil import esc
from .paths import M
from .render import shell


def hub_children_section(mod: dict) -> str:
    cards = []
    for c in mod["children"]:
        href = f'{mod["id"]}/{c["id"]}.html'
        cards.append(
            f'''        <a class="tree-card" href="{href}">
          <div class="num">{esc(c["id"])}</div>
          <h3>{esc(c["title"])}</h3>
          <p>{esc(c["summary"])}</p>
        </a>'''
        )
    return f'''
      <div class="section-rule"><h2 id="subtree">Design units in this module</h2></div>
      <p class="prose reveal" style="color:var(--ivory-mute);max-width:40rem">Each card is a focused design page (what / how / why + sources). Full tree: <a href="sitemap.html">Sitemap</a>.</p>
      <div class="catalog reveal">
{chr(10).join(cards)}
      </div>
'''


def inject_hub_subtree(hub_name: str, mod: dict) -> None:
    path = M / hub_name
    if not path.exists():
        return
    t = path.read_text()
    section = hub_children_section(mod).strip() + "\n\n      "
    if '<nav class="pager">' in t:
        if 'id="subtree"' in t:
            t = re.sub(
                r'<div class="section-rule"><h2 id="subtree">[\s\S]*?(?=<nav class="pager">)',
                section,
                t,
                count=1,
            )
        else:
            t = t.replace('<nav class="pager">', section + '<nav class="pager">', 1)
    else:
        t = t.replace("</article>", section + "\n    </article>", 1)

    if "nav-tree.js" not in t:
        t = t.replace(
            '<script defer src="../js/glossary-data.js"></script>',
            '<script defer src="../js/nav-tree.js"></script>\n  <script defer src="../js/glossary-data.js"></script>',
        )
    if 'id="toc-mount"' not in t:
        t = t.replace(
            '<div class="progress"',
            '<div id="toc-mount"></div>\n  <div class="progress"',
            1,
        )
        t = t.replace('<nav class="toc"', '<nav class="toc toc-legacy"', 1)
    if "data-depth=" not in t:
        t = t.replace('data-root=".."', 'data-root=".." data-depth="1"', 1)
    path.write_text(t)


def write_module_index(mod: dict) -> None:
    if mod.get("hub"):
        return
    cards = []
    for c in mod["children"]:
        href = f'{c["id"]}.html'
        cards.append(
            f'''        <a class="tree-card" href="{href}">
          <div class="num">{esc(c["id"])}</div>
          <h3>{esc(c["title"])}</h3>
          <p>{esc(c["summary"])}</p>
        </a>'''
        )
    cards_html = f'''
      <div class="section-rule"><h2 id="subtree">Design units in this module</h2></div>
      <p class="prose reveal" style="color:var(--ivory-mute);max-width:40rem">Each card is a focused design page. Full tree: <a href="../sitemap.html">Sitemap</a>.</p>
      <div class="catalog reveal">
{chr(10).join(cards)}
      </div>
'''
    body = f'''
      <p class="crumb"><a href="../../index.html">Monograph</a> ·
        <a href="../sitemap.html">Tree</a> · {esc(mod["title"])}</p>
      <header class="chapter-head reveal">
        <div class="chapter-num">▸</div>
        <div>
          <p class="chapter-kicker">Module hub</p>
          <h1>{esc(mod["title"])}</h1>
        </div>
      </header>
      <div class="thesis-box reveal">
        <div class="label">Module</div>
        <p>Design units under <strong>{esc(mod["id"])}</strong> — what it is, how it is implemented, why the design exists.</p>
      </div>
{cards_html}
'''
    out = M / mod["id"] / "index.html"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(shell(mod["title"], mod["title"] + " hub", f'{mod["id"]}/index', body, depth=2))
