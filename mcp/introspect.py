"""Engine introspection — codebase analysis and runtime pipeline inspection.

Static tools work offline (no engine needed). Runtime tools require the engine.
"""

import re
from pathlib import Path

PROJECT_ROOT = Path(__file__).parent.parent

# ─────────────────────────────────────────────────────────────────────────────
# Static analysis helpers
# ─────────────────────────────────────────────────────────────────────────────

def _scan_dir(path: Path, extensions: set[str]) -> list[dict]:
    """Scan directory for files with given extensions. Returns [{path, size, lines}]."""
    results = []
    if not path.exists():
        return results
    for f in sorted(path.rglob("*")):
        if f.is_file() and f.suffix in extensions:
            try:
                lines = f.read_text(encoding="utf-8", errors="replace").count("\n") + 1
            except Exception:
                lines = 0
            results.append({
                "path": str(f.relative_to(PROJECT_ROOT)),
                "size": f.stat().st_size,
                "lines": lines,
            })
    return results


def _parse_shader_includes(shader_path: Path) -> list[str]:
    """Extract #include paths from a GLSL shader."""
    includes = []
    try:
        text = shader_path.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r'#include\s+"([^"]+)"', text):
            includes.append(m.group(1))
    except Exception:
        pass
    return includes


def _parse_cpp_methods(header_path: Path, class_name: str) -> list[dict]:
    """Extract public method signatures from a C++ header."""
    methods = []
    try:
        text = header_path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return methods

    # Find class block
    class_match = re.search(rf"class\s+{class_name}\s*[^{{]*\{{", text)
    if not class_match:
        return methods

    class_body = text[class_match.end():]
    # Track public/private/protected sections
    in_public = False
    brace_depth = 1

    for line in class_body.split("\n"):
        stripped = line.strip()
        brace_depth += stripped.count("{") - stripped.count("}")
        if brace_depth <= 0:
            break
        if stripped.startswith("public:"):
            in_public = True
            continue
        if stripped.startswith(("private:", "protected:")):
            in_public = False
            continue
        if not in_public:
            continue

        # Match method declarations (not constructors/destructors/comments)
        m = re.match(
            r"(?:virtual\s+|static\s+)?"
            r"([\w:*&<>\s]+?)\s+"
            r"(\w+)\s*\(([^)]*)\)\s*"
            r"(const)?\s*"
            r"(?:override)?\s*;",
            stripped,
        )
        if m and not stripped.startswith("//"):
            ret_type = m.group(1).strip()
            name = m.group(2)
            params = m.group(3).strip()
            const = bool(m.group(4))
            # Skip internal/private-looking methods
            if name.startswith("_"):
                continue
            methods.append({
                "name": name,
                "return_type": ret_type,
                "params": params,
                "const": const,
            })
    return methods


def _find_render_passes() -> list[dict]:
    """Discover all render pass classes by scanning hpp files."""
    passes_dir = PROJECT_ROOT / "src" / "renderer" / "passes"
    passes = []
    if not passes_dir.exists():
        return passes

    for f in sorted(passes_dir.rglob("*.hpp")):
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        for m in re.finditer(r"class\s+(\w+Pass)\s*:\s*public\s+RenderPassBase", text):
            passes.append({
                "class": m.group(1),
                "header": str(f.relative_to(PROJECT_ROOT)),
                "source": str(f.with_suffix(".cpp").relative_to(PROJECT_ROOT)),
            })
    return passes


# ─────────────────────────────────────────────────────────────────────────────
# Registration
# ─────────────────────────────────────────────────────────────────────────────

def register(mcp, bridge):
    """Register introspection tools on the MCP server."""

    @mcp.tool
    def introspect_codebase(domain: str | None = None) -> dict:
        """Analyze engine codebase structure. Returns file counts, line counts,
        and key components. Works offline.

        domain: 'renderer', 'physics', 'audio', 'shaders', 'gdscript', 'all'
        If None, returns a high-level overview."""
        domain_map = {
            "renderer": (PROJECT_ROOT / "src" / "renderer", {".cpp", ".hpp", ".h"}),
            "physics": (PROJECT_ROOT / "src" / "physics", {".cpp", ".hpp", ".h"}),
            "audio": (PROJECT_ROOT / "src" / "audio", {".cpp", ".hpp", ".h"}),
            "shaders": (PROJECT_ROOT / "shaders", {".vert", ".frag", ".comp", ".geom", ".glsl"}),
            "gdscript": (PROJECT_ROOT / "godot_editor" / "project", {".gd"}),
        }

        if domain and domain != "all" and domain in domain_map:
            path, exts = domain_map[domain]
            files = _scan_dir(path, exts)
            return {
                "domain": domain,
                "file_count": len(files),
                "total_lines": sum(f["lines"] for f in files),
                "files": files,
            }

        # Overview mode
        overview = {}
        for name, (path, exts) in domain_map.items():
            files = _scan_dir(path, exts)
            overview[name] = {
                "file_count": len(files),
                "total_lines": sum(f["lines"] for f in files),
            }

        render_passes = _find_render_passes()
        return {
            "domains": overview,
            "render_passes": [p["class"] for p in render_passes],
            "render_pass_count": len(render_passes),
            "project_root": str(PROJECT_ROOT),
        }

    @mcp.tool
    def introspect_shaders() -> dict:
        """Map all GLSL shaders with #include dependencies. Returns the shader
        dependency graph showing which includes affect which compiled shaders.
        Works offline."""
        shaders_dir = PROJECT_ROOT / "shaders"
        if not shaders_dir.exists():
            return {"error": "shaders/ directory not found"}

        shaders = []
        include_graph: dict[str, list[str]] = {}  # include → [dependents]

        for ext in ("*.vert", "*.frag", "*.comp", "*.geom"):
            for f in sorted(shaders_dir.rglob(ext)):
                rel = str(f.relative_to(PROJECT_ROOT))
                # Build SPV output name: path separators → underscore
                spv_name = str(f.relative_to(shaders_dir)).replace("/", "_").replace("\\", "_") + ".spv"
                includes = _parse_shader_includes(f)

                shader_info = {
                    "source": rel,
                    "spv": f"bin/shaders/{spv_name}",
                    "type": f.suffix[1:],
                    "includes": includes,
                }
                shaders.append(shader_info)

                for inc in includes:
                    include_graph.setdefault(inc, []).append(rel)

        # Also list include files
        includes_dir = shaders_dir / "includes"
        include_files = []
        if includes_dir.exists():
            for f in sorted(includes_dir.rglob("*.glsl")):
                rel = str(f.relative_to(shaders_dir))
                include_files.append({
                    "path": rel,
                    "dependents": include_graph.get(rel, []),
                })

        return {
            "shader_count": len(shaders),
            "include_count": len(include_files),
            "shaders": shaders,
            "includes": include_files,
        }

    @mcp.tool
    def introspect_api(class_name: str = "OhaoViewport") -> dict:
        """Extract public API methods from a C++ class header. Parses method
        signatures, return types, and parameters. Works offline.

        class_name: 'OhaoViewport', 'OhaoPhysicsBody', 'OhaoMeshInstance',
                    'DeferredRenderer', 'RenderPassBase'"""
        search_paths = {
            "OhaoViewport": PROJECT_ROOT / "godot_editor" / "src" / "ohao_viewport.h",
            "OhaoPhysicsBody": PROJECT_ROOT / "godot_editor" / "src" / "ohao_physics_body.h",
            "OhaoMeshInstance": PROJECT_ROOT / "godot_editor" / "src" / "ohao_mesh_instance.h",
            "DeferredRenderer": PROJECT_ROOT / "src" / "renderer" / "passes" / "deferred_renderer.hpp",
            "RenderPassBase": PROJECT_ROOT / "src" / "renderer" / "passes" / "render_pass_base.hpp",
        }

        header = search_paths.get(class_name)
        if not header:
            # Try to find by scanning
            for p in (PROJECT_ROOT / "src").rglob("*.hpp"):
                if class_name.lower() in p.stem.lower():
                    header = p
                    break
            for p in (PROJECT_ROOT / "godot_editor" / "src").rglob("*.h"):
                if class_name.lower() in p.stem.lower():
                    header = p
                    break

        if not header or not header.exists():
            return {"error": f"Header not found for class '{class_name}'"}

        methods = _parse_cpp_methods(header, class_name)

        # Group by domain heuristic
        groups: dict[str, list] = {}
        for m in methods:
            name = m["name"]
            if "camera" in name.lower():
                group = "camera"
            elif "physics" in name.lower() or "actor" in name.lower():
                group = "physics"
            elif any(k in name.lower() for k in ("bloom", "ssao", "ssgi", "ssr", "fog", "tonemap", "dof", "taa")):
                group = "effects"
            elif any(k in name.lower() for k in ("terrain", "water", "foliage", "decal")):
                group = "environment"
            elif any(k in name.lower() for k in ("add_", "remove_", "clear_", "sync", "scene")):
                group = "scene"
            elif any(k in name.lower() for k in ("sound", "audio", "music")):
                group = "audio"
            else:
                group = "other"
            groups.setdefault(group, []).append(m)

        return {
            "class": class_name,
            "header": str(header.relative_to(PROJECT_ROOT)),
            "method_count": len(methods),
            "groups": groups,
        }

    @mcp.tool
    def introspect_render_passes() -> dict:
        """List all render pass classes with their source files and base class.
        Includes the deferred pipeline execution order. Works offline."""
        passes = _find_render_passes()

        # Known execution order from deferred_renderer.cpp::render()
        execution_order = [
            "CSMPass", "GBufferPass", "TerrainPass", "FoliagePass", "DecalPass",
            "SSAOPass", "SSGIPass", "DeferredLightingPass",
            "CloudPass", "CloudShadowPass", "SkyPass",
            "RainPass", "SnowPass", "SandPass",
            "GodRaysPass", "AuroraPass", "RainbowPass",
            "CausticsPass", "RipplePass", "WaterPass", "UnderwaterPass",
            "ParticleSystem", "PostProcessingPipeline", "GizmoPass",
        ]

        pass_map = {p["class"]: p for p in passes}
        ordered = []
        for i, name in enumerate(execution_order):
            entry = pass_map.pop(name, {"class": name, "header": "?", "source": "?"})
            entry["order"] = i + 1
            ordered.append(entry)
        # Add any passes not in known order
        for p in pass_map.values():
            p["order"] = len(ordered) + 1
            ordered.append(p)

        return {
            "pass_count": len(ordered),
            "passes": ordered,
        }

    @mcp.tool
    def introspect_pipeline() -> dict:
        """Get runtime render pipeline state: which passes are active, resolution,
        frame count, and timing. Requires engine running."""
        if not bridge.is_connected():
            return {"error": "Engine not running. Use introspect_render_passes() for offline info."}
        return bridge.get("/introspect/pipeline")

    @mcp.tool
    def introspect_perf() -> dict:
        """Get runtime performance metrics: frame time, per-pass GPU timing,
        draw calls, triangle count. Requires engine running."""
        if not bridge.is_connected():
            return {"error": "Engine not running."}
        return bridge.get("/introspect/perf")
