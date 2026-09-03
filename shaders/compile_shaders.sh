#!/bin/bash
# Shader compilation script for OHAO Engine
# Uses glslangValidator for include support (-I flag)
#
# Directory structure:
#   core/       - Main rendering shaders (forward.vert/frag)
#   shadow/     - Shadow rendering shaders (shadow_depth.vert/frag)
#   postprocess/- Post-processing shaders (future)
#   compute/    - Compute shaders (future)
#   includes/   - Shared GLSL modules
#   debug/      - Debug visualization shaders (future)

SHADER_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SHADER_DIR}/../build/shaders"
GODOT_DIR="${SHADER_DIR}/../godot_editor/project/bin/shaders"

# Create output directories
mkdir -p "$BUILD_DIR"
mkdir -p "$GODOT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m' # No Color

echo "==================================="
echo "OHAO Engine Shader Compilation"
echo "==================================="
echo "  Source: $SHADER_DIR"
echo "  Output: $BUILD_DIR"
echo ""

compile_shader() {
    local input="$1"
    local output="$2"
    local name=$(basename "$input")
    local dir=$(dirname "$input" | sed "s|$SHADER_DIR/||")

    # Build with includes from shader directory root
    if glslangValidator -V -I"$SHADER_DIR" -o "$output" "$input" 2>&1; then
        echo -e "  ${GREEN}OK${NC}    $dir/$name"
        # Also copy to Godot project
        cp "$output" "$GODOT_DIR/"
        return 0
    else
        echo -e "  ${RED}FAIL${NC}  $dir/$name"
        return 1
    fi
}

# Track errors
ERRORS=0

# Compile shaders from all subdirectories
for subdir in core shadow postprocess compute debug; do
    if [ -d "$SHADER_DIR/$subdir" ]; then
        # Check if there are any shader files
        has_shaders=false
        for shader in "$SHADER_DIR/$subdir"/*.vert "$SHADER_DIR/$subdir"/*.frag "$SHADER_DIR/$subdir"/*.comp "$SHADER_DIR/$subdir"/*.geom; do
            if [ -f "$shader" ]; then
                has_shaders=true
                break
            fi
        done

        if [ "$has_shaders" = true ]; then
            echo -e "${YELLOW}[$subdir]${NC}"

            # Compile vertex shaders
            for shader in "$SHADER_DIR/$subdir"/*.vert; do
                [ -f "$shader" ] || continue
                name=$(basename "$shader")
                # Output name includes subdirectory prefix for uniqueness
                compile_shader "$shader" "$BUILD_DIR/${subdir}_${name}.spv" || ((ERRORS++))
            done

            # Compile fragment shaders
            for shader in "$SHADER_DIR/$subdir"/*.frag; do
                [ -f "$shader" ] || continue
                name=$(basename "$shader")
                compile_shader "$shader" "$BUILD_DIR/${subdir}_${name}.spv" || ((ERRORS++))
            done

            # Compile geometry shaders
            for shader in "$SHADER_DIR/$subdir"/*.geom; do
                [ -f "$shader" ] || continue
                name=$(basename "$shader")
                compile_shader "$shader" "$BUILD_DIR/${subdir}_${name}.spv" || ((ERRORS++))
            done

            # Compile compute shaders
            for shader in "$SHADER_DIR/$subdir"/*.comp; do
                [ -f "$shader" ] || continue
                name=$(basename "$shader")
                compile_shader "$shader" "$BUILD_DIR/${subdir}_${name}.spv" || ((ERRORS++))
            done
        fi
    fi
done

# Also compile any shaders in the root directory (legacy compatibility)
has_root_shaders=false
for shader in "$SHADER_DIR"/*.vert "$SHADER_DIR"/*.frag; do
    if [ -f "$shader" ]; then
        has_root_shaders=true
        break
    fi
done

if [ "$has_root_shaders" = true ]; then
    echo -e "${YELLOW}[root]${NC}"
    for shader in "$SHADER_DIR"/*.vert "$SHADER_DIR"/*.frag; do
        [ -f "$shader" ] || continue
        name=$(basename "$shader")
        compile_shader "$shader" "$BUILD_DIR/${name}.spv" || ((ERRORS++))
    done
fi

echo ""
echo "==================================="
if [ $ERRORS -eq 0 ]; then
    echo -e "${GREEN}All shaders compiled successfully!${NC}"
else
    echo -e "${RED}$ERRORS shader(s) failed to compile${NC}"
    exit 1
fi
