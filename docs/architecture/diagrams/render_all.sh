#!/bin/bash
# Render all DOT diagrams to PNG
# Requires: brew install graphviz

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "Rendering DOT diagrams..."

for dotfile in *.dot; do
    if [ -f "$dotfile" ]; then
        basename="${dotfile%.dot}"
        echo "  $dotfile -> ${basename}.png"
        dot -Tpng "$dotfile" -o "${basename}.png"
        dot -Tsvg "$dotfile" -o "${basename}.svg"
    fi
done

echo "Done! Generated PNG and SVG files."
ls -la *.png *.svg 2>/dev/null
