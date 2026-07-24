"""Orchestrate full site tree generation."""
from __future__ import annotations

from .htmlutil import esc
from .inject import inject_hub_subtree, write_module_index
from .nav import generate_nav_js
from .paths import M, SITE
from .render import render_leaf, shell
from .tree_data import TREE


def ensure_css() -> None:
    css_path = SITE / "styles.css"
    css = css_path.read_text()
    extras = """
/* Unit diagrams */
.diagram-plate { padding: 0.5rem; overflow-x: auto; }
.diagram-plate svg.unit-diagram { width: 100%; max-width: 44rem; height: auto; display: block; margin: 0 auto; }
.code-block {
  background: var(--paper); border: 1px solid var(--rule); border-radius: 4px;
  padding: 1rem 1.1rem; overflow-x: auto; font-family: var(--font-mono);
  font-size: 0.78rem; line-height: 1.45; color: var(--ivory-dim);
  white-space: pre; margin: 0.75rem 0 1.25rem;
}
.code-block code { font-family: inherit; color: inherit; background: none; padding: 0; }
"""
    if ".diagram-plate" not in css:
        css_path.write_text(css + extras)
    if ".toc-branch" not in css:
        # collapse styles already added earlier — skip
        pass


def build() -> int:
    ensure_css()
    count = 0
    for mod in TREE:
        mod_dir = M / mod["id"]
        mod_dir.mkdir(parents=True, exist_ok=True)
        siblings = mod["children"]
        for idx, c_raw in enumerate(siblings):
            body = render_leaf(mod["id"], c_raw, mod.get("hub"))
            page_id = f'{mod["id"]}/{c_raw["id"]}'
            prev = siblings[idx - 1] if idx > 0 else None
            nxt = siblings[idx + 1] if idx + 1 < len(siblings) else None
            pager = '<nav class="pager">'
            if prev:
                pager += (
                    f'<a href="{prev["id"]}.html"><div class="label">← Prev</div>'
                    f'<div class="title">{esc(prev["title"])}</div></a>'
                )
            else:
                hub = f'../{mod["hub"]}' if mod.get("hub") else "index.html"
                pager += (
                    f'<a href="{hub}"><div class="label">← Hub</div>'
                    f'<div class="title">{esc(mod["title"])}</div></a>'
                )
            if nxt:
                pager += (
                    f'<a class="next" href="{nxt["id"]}.html"><div class="label">Next →</div>'
                    f'<div class="title">{esc(nxt["title"])}</div></a>'
                )
            else:
                # next module first leaf or sitemap
                pager += (
                    '<a class="next" href="../sitemap.html"><div class="label">Tree →</div>'
                    '<div class="title">Sitemap</div></a>'
                )
            pager += "</nav>"
            body = body + "\n" + pager
            html_out = shell(
                f'{mod["title"]} · {c_raw["title"]}',
                c_raw["summary"],
                page_id,
                body,
                depth=2,
            )
            (mod_dir / f'{c_raw["id"]}.html').write_text(html_out)
            count += 1
        if mod.get("hub"):
            inject_hub_subtree(mod["hub"], mod)
        else:
            write_module_index(mod)

    (SITE / "js" / "nav-tree.js").write_text(generate_nav_js())
    print("nav-tree.js written")

    # sitemap
    lines = [
        '<p class="crumb"><a href="../index.html">Monograph</a> · Full sitemap</p>',
        '<header class="chapter-head reveal"><div class="chapter-num">∑</div><div>',
        '<p class="chapter-kicker">Codebase coverage</p><h1>Sitemap</h1></div></header>',
        '<div class="thesis-box reveal"><div class="label">Scope</div>',
        "<p>Every design unit maps to real paths under <code>ohao/</code> and <code>shaders/</code>. "
        "Non-product research trees are omitted from the public face. "
        "Each leaf documents <strong>what</strong>, <strong>how</strong>, and <strong>why</strong>.</p></div>",
    ]
    total = 0
    for mod in TREE:
        lines.append(f'<h2 class="gloss-group">{esc(mod["title"])}</h2><ul class="prose">')
        if mod.get("hub"):
            lines.append(f'<li><a href="{mod["hub"]}"><strong>Hub</strong> — {esc(mod["hub"])}</a></li>')
        else:
            lines.append(f'<li><a href="{mod["id"]}/index.html"><strong>Hub</strong></a></li>')
        for c in mod["children"]:
            total += 1
            lines.append(
                f'<li><a href="{mod["id"]}/{c["id"]}.html">{esc(c["title"])}</a> — {esc(c["summary"])}</li>'
            )
        lines.append("</ul>")
    lines.append(f'<p class="prose" style="color:var(--ivory-mute)">{total} design units generated.</p>')
    (M / "sitemap.html").write_text(
        shell("Sitemap", "Full monograph tree", "sitemap", "\n".join(lines), depth=1)
    )

    idx = SITE / "index.html"
    it = idx.read_text()
    if "nav-tree.js" not in it:
        it = it.replace(
            '<script defer src="js/glossary-data.js"></script>',
            '<script defer src="js/nav-tree.js"></script>\n  <script defer src="js/glossary-data.js"></script>',
        )
    if 'id="toc-mount"' not in it:
        it = it.replace('<div class="progress"', '<div id="toc-mount"></div>\n  <div class="progress"', 1)
        it = it.replace('<nav class="toc"', '<nav class="toc toc-legacy"', 1)
    if "data-depth=" not in it:
        it = it.replace("<body", '<body data-page="home" data-depth="0" data-root="."', 1)
    idx.write_text(it)

    print(f"Generated {count} leaf pages across {len(TREE)} modules")
    print("Sitemap: site/m/sitemap.html")
    return count
