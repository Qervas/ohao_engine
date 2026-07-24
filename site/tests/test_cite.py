import sys
import unittest
from pathlib import Path

SITE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SITE / "tools"))

from monograph import cite  # noqa: E402
from monograph.paths import ROOT  # noqa: E402


class ResolveCitationTest(unittest.TestCase):
    def test_unique_match_returns_line_and_text(self):
        line, text = cite.resolve_citation(
            "shaders/includes/brdf/brdf_ggx.glsl",
            "float Ems = (1.0 - E_o) * (1.0 - E_i);",
        )
        self.assertEqual(line, 159)
        self.assertIn("Ems", text)

    def test_missing_substring_raises(self):
        with self.assertRaises(cite.CitationError):
            cite.resolve_citation(
                "shaders/includes/brdf/brdf_ggx.glsl",
                "this text does not exist anywhere zzz",
            )

    def test_ambiguous_substring_raises_and_lists_lines(self):
        with self.assertRaises(cite.CitationError) as ctx:
            cite.resolve_citation(
                "shaders/includes/pbr_unpack.glsl",
                "if (roughness >= 10.0) roughness -= 10.0;",
            )
        msg = str(ctx.exception)
        self.assertIn("11", msg)  # first of the two matching lines listed

    def test_missing_file_raises(self):
        with self.assertRaises(cite.CitationError):
            cite.resolve_citation("no/such/file.glsl", "anything")

    def test_chip_contains_path_line_and_github_link(self):
        html = cite.citation_chip_html(
            "shaders/includes/material/material_types.glsl",
            "return mix(dielectricF0, surface.albedo, surface.metallic);",
        )
        self.assertIn("material_types.glsl:138", html)
        self.assertIn(cite.GITHUB_BLOB_BASE, html)
        self.assertIn("#L138", html)
        self.assertIn("citation-chip", html)


if __name__ == "__main__":
    unittest.main(verbosity=2)
