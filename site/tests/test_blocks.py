import sys
import unittest
from pathlib import Path

SITE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SITE / "tools"))

from monograph import blocks  # noqa: E402


class FrontmatterTest(unittest.TestCase):
    def test_split_frontmatter_parses_scalars_and_lists(self):
        fm, body = blocks.split_frontmatter(
            "---\n"
            "module: materials\n"
            "id: pbr-model\n"
            "standard: v2\n"
            "figures: [ggx_multiscatter_energy]\n"
            "---\n"
            "\n## Heading\n\nprose\n"
        )
        self.assertEqual(fm["module"], "materials")
        self.assertEqual(fm["standard"], "v2")
        self.assertEqual(fm["figures"], ["ggx_multiscatter_energy"])
        self.assertIn("## Heading", body)


class RenderBodyTest(unittest.TestCase):
    def test_heading_becomes_section_and_appears_in_headings(self):
        html, headings, cited = blocks.render_body(
            "---\nid: x\n---\n\n## The lie artists can paint\n\nsome prose here\n"
        )
        self.assertIn('id="the-lie-artists-can-paint"', html)
        self.assertIn("<h2", html)
        self.assertIn(("the-lie-artists-can-paint", "The lie artists can paint"), headings)
        self.assertIn("some prose here", html)

    def test_cite_block_renders_chip_and_collects_path(self):
        html, headings, cited = blocks.render_body(
            '---\nid: x\n---\n\n## S\n\nlead prose\n'
            '{{cite shaders/includes/brdf/brdf_ggx.glsl "float Ems = (1.0 - E_o) * (1.0 - E_i);"}}\n'
        )
        self.assertIn("citation-chip", html)
        self.assertIn("brdf_ggx.glsl:159", html)
        self.assertIn("shaders/includes/brdf/brdf_ggx.glsl", cited)

    def test_math_block_renders_katex_delimiters(self):
        html, _, _ = blocks.render_body(
            "---\nid: x\n---\n\n## M\n\nprose\n\n$$F_0 = 0.04$$\n"
        )
        self.assertIn("math-block", html)
        self.assertIn("F_0 = 0.04", html)

    def test_why_and_key_callouts(self):
        html, _, _ = blocks.render_body(
            "---\nid: x\n---\n\n## S\n\n:::why\nbecause the alternative fireflies\n:::\n"
            "\n:::key\nthe one thing to remember\n:::\n"
        )
        self.assertIn("why-callout", html)
        self.assertIn("because the alternative fireflies", html)
        self.assertIn("key-idea", html)
        self.assertIn("the one thing to remember", html)

    def test_figure_renders_with_caption(self):
        html, _, _ = blocks.render_body(
            '---\nid: x\n---\n\n## S\n\n{{figure ggx_multiscatter_energy "Measured energy loss vs roughness."}}\n'
        )
        self.assertIn("unit-figure", html)
        self.assertIn("ggx_multiscatter_energy.svg", html)
        self.assertIn("Measured energy loss vs roughness.", html)

    def test_legacy_sources_of_truth_line_is_dropped(self):
        html, _, _ = blocks.render_body(
            "---\nid: x\n---\n\n## Contracts\n\n- Sources of truth: shaders/foo.glsl\n- real invariant holds\n"
        )
        self.assertNotIn("Sources of truth", html)
        self.assertIn("real invariant holds", html)

    def test_empty_section_is_not_rendered(self):
        html, headings, _ = blocks.render_body(
            "---\nid: x\n---\n\n## Empty\n\n## Full\n\nhas prose\n"
        )
        self.assertNotIn("Empty", html)
        self.assertIn("Full", html)


class MathGateTest(unittest.TestCase):
    def test_math_section_with_prose_is_ok(self):
        secs = blocks.iter_math_sections(
            "---\nid: x\n---\n\n## M\n\nexplaining prose\n\n$$a=b$$\n"
        )
        self.assertEqual(secs, [("m", True)])

    def test_math_section_without_prose_is_flagged(self):
        secs = blocks.iter_math_sections(
            "---\nid: x\n---\n\n## M\n\n$$a=b$$\n"
        )
        self.assertEqual(secs, [("m", False)])


if __name__ == "__main__":
    unittest.main(verbosity=2)
