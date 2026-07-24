"""OHAO monograph site generator.

Run: python3 site/tools/generate_tree.py
"""
from .paths import CONTENT, DIAGRAMS, M, ROOT, SITE
from .schema import page
from .tree_data import TREE

__all__ = ["ROOT", "SITE", "M", "CONTENT", "DIAGRAMS", "page", "TREE"]
