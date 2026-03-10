"""Hot-reload — swap shaders and scripts at runtime without restart.

Compiles GLSL to SPIR-V, copies to the runtime shader directory, and signals
the engine to reload. Validates before deploying.
"""

import shutil
import subprocess
import tempfile
from pathlib import Path

PROJECT_ROOT = Path(__file__).parent.parent
SHADER_BIN = PROJECT_ROOT / "godot_editor" / "project" / "bin" / "shaders"

# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def _find_glslc() -> str | None:
    """Find glslc shader compiler."""
    path = shutil.which("glslc")
    if path:
        return path
    vulkan_sdk = Path.home() / "VulkanSDK"
    if vulkan_sdk.exists():
        for glslc in vulkan_sdk.rglob("glslc.exe"):
            return str(glslc)
        for glslc in vulkan_sdk.rglob("glslc"):
            return str(glslc)
    return None


def _compile_shader(glslc: str, src_path: str, out_path: str) -> dict:
    """Compile a single shader. Returns {success, errors}."""
    try:
        result = subprocess.run(
            [
                glslc,
                "-I", str(PROJECT_ROOT / "shaders"),
                "-I", str(PROJECT_ROOT / "shaders" / "includes"),
                src_path,
                "-o", out_path,
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        return {
            "success": result.returncode == 0,
            "errors": result.stderr,
        }
    except Exception as e:
        return {"success": False, "errors": str(e)}


# ─────────────────────────────────────────────────────────────────────────────
# Registration
# ─────────────────────────────────────────────────────────────────────────────

def register(mcp, bridge):
    """Register hot-reload tools on the MCP server."""

    @mcp.tool
    def hot_reload_shader(pass_name: str, shader_path: str) -> dict:
        """Compile a GLSL shader, deploy the SPV, and signal the engine to reload.

        pass_name: render pass name (e.g., 'SSAO', 'CloudPass', 'WaterPass')
        shader_path: path to .glsl/.comp/.vert/.frag relative to project root

        Flow: compile → validate → backup old SPV → deploy new SPV → signal engine.
        If compilation fails, nothing is deployed."""
        glslc = _find_glslc()
        if not glslc:
            return {"error": "glslc not found. Install Vulkan SDK."}

        src = PROJECT_ROOT / shader_path
        if not src.exists():
            return {"error": f"Shader source not found: {shader_path}"}

        # Determine SPV output name (mirrors CMake convention)
        shaders_dir = PROJECT_ROOT / "shaders"
        try:
            rel = src.relative_to(shaders_dir)
        except ValueError:
            rel = Path(src.name)
        spv_name = str(rel).replace("/", "_").replace("\\", "_") + ".spv"

        # Compile to temp first
        with tempfile.NamedTemporaryFile(suffix=".spv", delete=False) as tmp:
            tmp_path = tmp.name

        compile_result = _compile_shader(glslc, str(src), tmp_path)
        if not compile_result["success"]:
            Path(tmp_path).unlink(missing_ok=True)
            return {
                "reloaded": False,
                "stage": "compilation",
                "errors": compile_result["errors"],
            }

        # Backup and deploy
        dest = SHADER_BIN / spv_name
        backup = None
        if dest.exists():
            backup = dest.with_suffix(".spv.bak")
            shutil.copy2(dest, backup)

        shutil.copy2(tmp_path, dest)
        Path(tmp_path).unlink(missing_ok=True)

        # Also copy to build/shaders for consistency
        build_dest = PROJECT_ROOT / "build" / "shaders" / spv_name
        build_dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(dest, build_dest)

        # Signal engine to reload
        engine_result = {"signaled": False}
        if bridge.is_connected():
            try:
                engine_result = bridge.post("/hot_reload/shader", {
                    "pass_name": pass_name,
                    "spv_path": str(dest),
                })
                engine_result["signaled"] = True
            except Exception as e:
                engine_result = {"signaled": False, "engine_error": str(e)}

        return {
            "reloaded": True,
            "shader": shader_path,
            "spv": spv_name,
            "deployed_to": str(dest),
            "backup": str(backup) if backup else None,
            "engine": engine_result,
        }

    @mcp.tool
    def hot_reload_shader_source(pass_name: str, glsl_code: str, shader_type: str = "comp") -> dict:
        """Write GLSL code to a temp file, compile, and hot-reload.

        pass_name: render pass name (e.g., 'SSAO')
        glsl_code: raw GLSL source code
        shader_type: 'comp', 'vert', or 'frag'

        Use this for quick shader experiments without modifying source files."""
        glslc = _find_glslc()
        if not glslc:
            return {"error": "glslc not found. Install Vulkan SDK."}

        # Write to temp
        suffix = f".{shader_type}"
        with tempfile.NamedTemporaryFile(mode="w", suffix=suffix, delete=False, encoding="utf-8") as tmp:
            tmp.write(glsl_code)
            src_path = tmp.name

        # Compile
        with tempfile.NamedTemporaryFile(suffix=".spv", delete=False) as out:
            out_path = out.name

        compile_result = _compile_shader(glslc, src_path, out_path)
        Path(src_path).unlink(missing_ok=True)

        if not compile_result["success"]:
            Path(out_path).unlink(missing_ok=True)
            return {
                "reloaded": False,
                "stage": "compilation",
                "errors": compile_result["errors"],
            }

        # Signal engine
        engine_result = {"signaled": False}
        if bridge.is_connected():
            try:
                engine_result = bridge.post("/hot_reload/shader", {
                    "pass_name": pass_name,
                    "spv_path": out_path,
                })
                engine_result["signaled"] = True
            except Exception as e:
                engine_result = {"signaled": False, "engine_error": str(e)}

        Path(out_path).unlink(missing_ok=True)

        return {
            "reloaded": True,
            "pass_name": pass_name,
            "shader_type": shader_type,
            "engine": engine_result,
        }

    @mcp.tool
    def hot_reload_script(script_path: str) -> dict:
        """Signal the engine to reload a GDScript file.

        script_path: path relative to project root
        (e.g., 'godot_editor/project/addons/ohao_helpers/ohao_server.gd')"""
        if not bridge.is_connected():
            return {"error": "Engine not running"}

        return bridge.post("/hot_reload/script", {"path": script_path})
