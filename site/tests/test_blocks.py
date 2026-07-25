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

    def test_section_merely_mentioning_source_map_still_renders(self):
        html, headings, _ = blocks.render_body(
            "---\nid: x\n---\n\n## Real Section\n\n"
            "This mentions the Source map: idea in passing.\n"
        )
        self.assertIn('id="real-section"', html)
        self.assertIn("<h2", html)
        self.assertIn(("real-section", "Real Section"), headings)

    def test_legacy_source_map_section_is_dropped(self):
        html, headings, _ = blocks.render_body(
            "---\nid: x\n---\n\n## Notes\n\nSource map:\n"
            "- `some/path.py`\n- `other/path.py`\n"
        )
        self.assertNotIn("Notes", html)
        self.assertNotIn("notes", [slug for slug, _ in headings])
        self.assertEqual(headings, [])

    def test_unterminated_math_block_raises(self):
        with self.assertRaises(ValueError):
            blocks.render_body(
                "---\nid: x\n---\n\n## S\n\nprose\n\n$$\nF_0 = 0.04\n"
            )

    def test_unterminated_why_callout_raises(self):
        with self.assertRaises(ValueError):
            blocks.render_body(
                "---\nid: x\n---\n\n## S\n\n:::why\nbecause reasons\n"
            )

    def test_unterminated_code_fence_raises(self):
        with self.assertRaises(ValueError):
            blocks.render_body(
                "---\nid: x\n---\n\n## S\n\n```glsl\nfloat x = 1.0;\n"
            )

    def test_duplicate_heading_slugs_are_deduped(self):
        html, headings, _ = blocks.render_body(
            "---\nid: x\n---\n\n## Overview\n\nfirst prose\n"
            "\n## Overview\n\nsecond prose\n"
        )
        self.assertEqual(
            headings,
            [("overview", "Overview"), ("overview-2", "Overview")],
        )
        self.assertIn('id="overview"', html)
        self.assertIn('id="overview-2"', html)

    def test_fenced_code_block_renders(self):
        html, _, _ = blocks.render_body(
            "---\nid: x\n---\n\n## S\n\nprose\n\n```glsl\nfloat x = 1.0;\n```\n"
        )
        self.assertIn('<pre class="code-block', html)
        self.assertIn("<code>", html)
        self.assertIn("float x = 1.0;", html)

    def test_multiline_math_block_renders(self):
        html, _, _ = blocks.render_body(
            "---\nid: x\n---\n\n## S\n\nprose\n\n$$\nF_0 = 0.04\n$$\n"
        )
        self.assertIn("math-block", html)
        self.assertIn("F_0 = 0.04", html)


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
