#!/usr/bin/env python3
"""Structural tests for the OHAO implementation monograph under site/.

These tests drive the real published files (not re-implemented stubs):
presence of plates, NEE walk pedagogy, audio module, Pages workflow,
and inverse-product exclusion.
"""
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

SITE = Path(__file__).resolve().parents[1]
ROOT = SITE.parent
M = SITE / "m"

sys.path.insert(0, str(SITE / "tools"))
from monograph import blocks as _blocks  # noqa: E402
from monograph import cite as _cite      # noqa: E402

CONTENT = SITE / "content" / "units"


class GateTest(unittest.TestCase):
    def _md_files(self):
        return sorted(CONTENT.rglob("*.md"))

    def test_no_sources_of_truth_filler(self):
        hits = [str(p.relative_to(SITE)) for p in M.rglob("*.html")
                if "Sources of truth:" in p.read_text(encoding="utf-8", errors="replace")]
        self.assertEqual(hits, [], f"filler present: {hits}")

    def test_no_api_dump(self):
        needle = "API / symbols the unit exposes or binds"
        hits = [str(p.relative_to(SITE)) for p in M.rglob("*.html")
                if needle in p.read_text(encoding="utf-8", errors="replace")]
        self.assertEqual(hits, [], f"api dump present: {hits}")

    def test_all_citations_resolve(self):
        errors = []
        for md in self._md_files():
            _fm, body = _blocks.split_frontmatter(md.read_text(encoding="utf-8"))
            for m in re.finditer(r'\{\{cite\s+(\S+)\s+"(.+?)"\}\}', body):
                try:
                    _cite.resolve_citation(m.group(1), m.group(2))
                except _cite.CitationError as e:
                    errors.append(f"{md.relative_to(CONTENT)}: {e}")
        self.assertEqual(errors, [], "\n".join(errors))

    def test_no_empty_section_rules(self):
        # Every <h2 id=...> must be followed by non-empty content before the next h2/footer.
        bad = []
        for p in M.rglob("*.html"):
            html = p.read_text(encoding="utf-8", errors="replace")
            for m in re.finditer(r'<h2 id="([^"]+)">.*?</h2></div>\s*(.*?)(?=<div class="section-rule"|</article>)',
                                 html, re.S):
                if not re.sub(r"<[^>]+>|\s", "", m.group(2)):
                    bad.append(f"{p.relative_to(SITE)}#{m.group(1)}")
        self.assertEqual(bad, [], f"empty sections: {bad}")

    def test_v2_math_blocks_are_explained(self):
        bad = []
        for md in self._md_files():
            fm, _ = _blocks.split_frontmatter(md.read_text(encoding="utf-8"))
            if fm.get("standard") != "v2":
                continue
            for slug, has_prose in _blocks.iter_math_sections(md.read_text(encoding="utf-8")):
                if not has_prose:
                    bad.append(f"{md.relative_to(CONTENT)}#{slug}")
        self.assertEqual(bad, [], f"unexplained math in v2 pages: {bad}")


class MonographStructureTest(unittest.TestCase):
    def test_binding_svg_plate_exists(self) -> None:
        svg = SITE / "assets" / "plates" / "pt_bindings.svg"
        self.assertTrue(svg.is_file(), f"missing full-page binding SVG: {svg}")
        text = svg.read_text(encoding="utf-8", errors="replace")
        self.assertIn("binding", text.lower())
        self.assertIn(">11<", text)  # light buffer slot called out
        self.assertIn("TLAS", text)
        # Shipped set 0 (path_tracer_descriptors.cpp): ReSTIR 29–34, DLSS-RR 35
        self.assertIn("ReSTIR", text)
        self.assertIn("29", text)
        self.assertIn("35", text)
        self.assertIn("DLSS-RR", text)
        # Must not claim 29–30 are NRD compose beauty on set 0
        self.assertNotIn("NRD composed HDR · tonemapped beauty", text)

    def test_gbuffer_layout_strip_exists(self) -> None:
        strip = SITE / "assets" / "plates" / "gbuffer_layout_strip.svg"
        self.assertTrue(strip.is_file(), strip)
        self.assertIn("NOT A LIVE CHANNEL DUMP", strip.read_text(encoding="utf-8"))

    def test_real_beauty_plate_exists(self) -> None:
        # Real cornell export from build/cornell_box (engine path)
        p = SITE / "assets" / "plates" / "cornell_beauty_16spp.png"
        self.assertTrue(p.is_file() and p.stat().st_size > 10_000, p)

    def test_path_tracer_nee_sphere_walk(self) -> None:
        html = (M / "path-tracer.html").read_text(encoding="utf-8")
        self.assertIn('id="nee-walk"', html)
        self.assertIn("lightType", html)
        self.assertIn("pt_raygen.rgen", html)
        # Side-by-side dual pane + honest line anchors
        self.assertIn("dual-pane", html)
        self.assertIn("L305", html)  # if (lightCount) — not the comment as the branch
        self.assertIn('class="ln">305</span>', html)
        self.assertIn('class="ln">310</span>', html)  # GPULight once
        self.assertIn("sphere", html.lower())
        # Distance d=lightDist in weight formula — not sphere radius r²
        self.assertIn("lightDist", html)
        self.assertIn("d^{2}", html)
        self.assertNotIn(
            r"w=\frac{|\mathbf{n}_L\!\cdot\!(-\omega)|\,A}{r^{2}}",
            html,
        )
        # Jargon first-use notes
        self.assertIn("Jargon · NEE", html)
        self.assertIn("Jargon · PDF", html)
        # Why callouts
        self.assertIn("why-callout", html)
        self.assertIn("Why multiply by light count", html)
        # Formula blocks
        self.assertIn("math-block", html)
        self.assertIn("L_{\\mathrm{NEE}}", html.replace(" ", ""))

    def test_conceptual_art_captioned(self) -> None:
        html = (M / "path-tracer.html").read_text(encoding="utf-8")
        self.assertIn("Conceptual illustration — not engine output", html)
        self.assertIn("Real engine output", html)
        concept = SITE / "assets" / "concept" / "nee_sphere_conceptual.jpg"
        self.assertTrue(concept.is_file(), concept)

    def test_audio_chapter_api_alignment(self) -> None:
        html = (M / "audio.html").read_text(encoding="utf-8")
        self.assertIn("SoundCategory", html)
        self.assertIn("playSoundAt", html)
        self.assertIn("setListenerPosition", html)
        self.assertIn("SFX", html)
        self.assertIn("Music", html)
        self.assertIn("Ambient", html)
        # Header source of truth still has these symbols
        hdr = (ROOT / "ohao" / "audio" / "audio_system.hpp").read_text(encoding="utf-8")
        self.assertIn("playSoundAt", hdr)
        self.assertIn("SoundCategory", hdr)

    def test_toc_links_audio(self) -> None:
        index = (SITE / "index.html").read_text(encoding="utf-8")
        self.assertIn("m/audio.html", index)
        arch = (M / "architecture.html").read_text(encoding="utf-8")
        self.assertIn("audio.html", arch)

    def test_no_inverse_product_content(self) -> None:
        banned = re.compile(
            r"inverse_fit|Diff-IR|MAPTEST|quality-plate|LABTEST|PHOTOTEST",
            re.I,
        )
        hits: list[str] = []
        for path in SITE.rglob("*.html"):
            text = path.read_text(encoding="utf-8", errors="replace")
            for m in banned.finditer(text):
                # allow mathematical "inverse" VP etc. — only product strings above
                hits.append(f"{path.relative_to(SITE)}:{m.group(0)}")
        self.assertEqual(hits, [], f"inverse product strings in site: {hits}")

    def test_pages_workflow_exists(self) -> None:
        wf = ROOT / ".github" / "workflows" / "pages.yml"
        self.assertTrue(wf.is_file(), wf)
        text = wf.read_text(encoding="utf-8")
        self.assertIn("site/", text)
        self.assertIn("deploy-pages", text)
        self.assertIn("inverse", text.lower())  # exclude inverse media
        deploy = SITE / "DEPLOY.md"
        self.assertTrue(deploy.is_file(), deploy)

    def test_codebase_tree_coverage(self) -> None:
        """Hierarchical design units cover ohao/ + active shaders/ (public face)."""
        sitemap = M / "sitemap.html"
        self.assertTrue(sitemap.is_file(), sitemap)
        sm = sitemap.read_text(encoding="utf-8")
        self.assertIn("design units", sm.lower())
        nav = (SITE / "js" / "nav-tree.js").read_text(encoding="utf-8")
        self.assertIn("OHAO_NAV_TREE", nav)
        # Core modules must have leaf subtrees
        for mod, leaf in (
            ("path-tracer", "raygen.html"),
            ("path-tracer", "host.html"),
            ("gpu", "renderer-facade.html"),
            ("deferred", "gbuffer.html"),
            ("denoise", "nrd.html"),
            ("shaders", "rt-shaders.html"),
            ("shaders", "postprocess-chain.html"),
            ("sampling", "mis.html"),
            ("materials", "ggx.html"),
            ("physics", "forces.html"),
            ("audio", "system.html"),
            ("scene", "actors.html"),
            ("hybrid", "shadow-technique.html"),
            ("core", "event-bus.html"),
        ):
            p = M / mod / leaf
            self.assertTrue(p.is_file(), p)
            html = p.read_text(encoding="utf-8")
            # Rich leaf sections (sources footer + nav script)
            self.assertIn("Source files", html)
            self.assertIn("nav-tree.js", html)
        # At least ~80 leaf pages under m/*/
        leaves = list(M.glob("*/*.html"))
        self.assertGreaterEqual(len(leaves), 80, f"expected full tree, got {len(leaves)}")
        # Hub injects design-unit cards
        gpu = (M / "gpu.html").read_text(encoding="utf-8")
        self.assertIn('id="subtree"', gpu)
        self.assertIn("tree-card", gpu)
        self.assertIn("gpu/renderer-facade.html", gpu)



    def test_glossary_page_and_tooltip_assets(self) -> None:
        gloss = M / "glossary.html"
        self.assertTrue(gloss.is_file(), gloss)
        html = gloss.read_text(encoding="utf-8")
        self.assertIn("GGX", html)
        self.assertIn("NEE", html)
        data = (SITE / "js" / "glossary-data.js").read_text(encoding="utf-8")
        self.assertIn("OHAO_GLOSSARY", data)
        self.assertIn("GGX:", data)
        self.assertIn("NEE:", data)
        js = (SITE / "js" / "glossary.js").read_text(encoding="utf-8")
        self.assertIn("term-tip", js)
        # Chapters load glossary scripts
        pt = (M / "path-tracer.html").read_text(encoding="utf-8")
        self.assertIn("glossary-data.js", pt)
        self.assertIn("glossary.js", pt)
        # TOC link
        self.assertIn("glossary.html", (SITE / "index.html").read_text(encoding="utf-8"))
        css = (SITE / "styles.css").read_text(encoding="utf-8")
        self.assertIn(".term-tip", css)



    def test_deep_module_workflows(self) -> None:
        """Each core module chapter documents a real workflow, not only a file list."""
        checks = {
            "gpu.html": ["createInstance", "initialize", "renderDeferred", "buildBLASTLAS", "FIG. GPU"],
            "scene.html": ["createActor", "initialize", "PhysicsWorld", "Hand-off"],
            "architecture.html": ["End-to-end", "Invariants", "ensureRTRenderer"],
            "deferred.html": ["renderDeferred", "m_renderGraph", "CSM", "GBuffer"],
            "path-tracer.html": ["ensureRTRenderer", "host-life", "nee-walk", "traceRays"],
            "physics.html": ["IPhysicsBackend", "step", "Jolt"],
            "audio.html": ["ma_engine_init", "playSoundAt", "setListenerPosition"],
        }
        for name, needles in checks.items():
            html = (M / name).read_text(encoding="utf-8")
            for n in needles:
                self.assertIn(n, html, f"{name} missing workflow marker: {n}")


if __name__ == "__main__":
    # Allow `python3 site/tests/test_monograph_structure.py`
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
