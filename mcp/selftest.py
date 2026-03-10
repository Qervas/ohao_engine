"""Self-test module — build validation, shader compilation, test execution.

AI agents use this to validate changes before deploying. All tools run
shell commands as subprocesses and parse the output.
"""

import subprocess
import shutil
from pathlib import Path

PROJECT_ROOT = Path(__file__).parent.parent

# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def _run(cmd: list[str], cwd: Path | None = None, timeout: int = 120) -> dict:
    """Run a command and return structured result."""
    try:
        result = subprocess.run(
            cmd,
            cwd=cwd or PROJECT_ROOT,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return {
            "success": result.returncode == 0,
            "returncode": result.returncode,
            "stdout": result.stdout[-4000:] if result.stdout else "",
            "stderr": result.stderr[-4000:] if result.stderr else "",
        }
    except subprocess.TimeoutExpired:
        return {"success": False, "error": f"Command timed out after {timeout}s"}
    except FileNotFoundError:
        return {"success": False, "error": f"Command not found: {cmd[0]}"}


def _find_glslc() -> str | None:
    """Find glslc shader compiler."""
    path = shutil.which("glslc")
    if path:
        return path
    # Check Vulkan SDK
    vulkan_sdk = Path.home() / "VulkanSDK"
    if vulkan_sdk.exists():
        for glslc in vulkan_sdk.rglob("glslc.exe"):
            return str(glslc)
        for glslc in vulkan_sdk.rglob("glslc"):
            return str(glslc)
    return None


# ─────────────────────────────────────────────────────────────────────────────
# Registration
# ─────────────────────────────────────────────────────────────────────────────

def register(mcp, bridge):
    """Register self-test tools on the MCP server."""

    @mcp.tool
    def test_compile_shader(shader_path: str) -> dict:
        """Compile a single GLSL shader to SPIR-V and validate it.

        shader_path: relative to project root (e.g., 'shaders/compute/ssao.comp')
        Returns compilation success/failure with error messages."""
        glslc = _find_glslc()
        if not glslc:
            return {"error": "glslc not found. Install Vulkan SDK."}

        src = PROJECT_ROOT / shader_path
        if not src.exists():
            return {"error": f"Shader not found: {shader_path}"}

        # Compile to temp output
        out = PROJECT_ROOT / "build" / "test_shader.spv"
        out.parent.mkdir(parents=True, exist_ok=True)

        result = _run([
            glslc,
            "-I", str(PROJECT_ROOT / "shaders"),
            "-I", str(PROJECT_ROOT / "shaders" / "includes"),
            str(src),
            "-o", str(out),
        ])

        # Clean up temp
        if out.exists():
            out.unlink()

        return {
            "shader": shader_path,
            "compiled": result["success"],
            "errors": result.get("stderr", ""),
        }

    @mcp.tool
    def test_compile_all_shaders() -> dict:
        """Compile all GLSL shaders via cmake. Returns pass/fail with error details."""
        build_dir = PROJECT_ROOT / "build"
        if not build_dir.exists():
            return {"error": "Build directory not found. Run cmake configure first."}

        result = _run(
            ["cmake", "--build", str(build_dir), "--target", "shaders", "--config", "Release"],
            timeout=60,
        )
        return {
            "compiled": result["success"],
            "output": result.get("stdout", ""),
            "errors": result.get("stderr", ""),
        }

    @mcp.tool
    def test_build_cpp(target: str | None = None) -> dict:
        """Build C++ engine core. Returns pass/fail with error details.

        target: specific cmake target (default: all). Examples:
        'ohao_renderer', 'ohao_physics', 'shaders'"""
        build_dir = PROJECT_ROOT / "build"
        if not build_dir.exists():
            return {"error": "Build directory not found. Run cmake configure first."}

        cmd = ["cmake", "--build", str(build_dir), "--config", "Release", "-j8"]
        if target:
            cmd.extend(["--target", target])

        result = _run(cmd, timeout=300)

        # Parse error count from output
        errors = []
        for line in (result.get("stderr", "") + result.get("stdout", "")).split("\n"):
            if ": error " in line.lower() or ": fatal error" in line.lower():
                errors.append(line.strip())

        return {
            "built": result["success"],
            "target": target or "all",
            "error_count": len(errors),
            "errors": errors[:20],  # First 20 errors
            "output": result.get("stdout", "")[-2000:],
        }

    @mcp.tool
    def test_build_gdext() -> dict:
        """Build GDExtension DLL via scons. Returns pass/fail with error details."""
        gdext_dir = PROJECT_ROOT / "godot_editor"
        if not gdext_dir.exists():
            return {"error": "godot_editor/ directory not found"}

        result = _run(
            ["scons", "platform=windows", "-j8"],
            cwd=gdext_dir,
            timeout=300,
        )

        errors = []
        for line in (result.get("stderr", "") + result.get("stdout", "")).split("\n"):
            if ": error " in line.lower() or ": fatal error" in line.lower():
                errors.append(line.strip())

        return {
            "built": result["success"],
            "error_count": len(errors),
            "errors": errors[:20],
            "output": result.get("stdout", "")[-2000:],
        }

    @mcp.tool
    def test_run_suite(suite: str) -> dict:
        """Run a C++ test suite. Returns pass/fail count and details.

        suite: 'physics', 'audio', 'force_generators', 'engine'"""
        exe_map = {
            "physics": "physics_backend_tests",
            "audio": "audio_tests",
            "force_generators": "force_generator_tests",
            "engine": "engine_tests",
        }

        if suite not in exe_map:
            return {"error": f"Unknown suite '{suite}'. Available: {', '.join(exe_map.keys())}"}

        exe_name = exe_map[suite]
        exe_path = PROJECT_ROOT / "build" / "Release" / f"{exe_name}.exe"
        if not exe_path.exists():
            # Try without Release subdir
            exe_path = PROJECT_ROOT / "build" / f"{exe_name}.exe"
        if not exe_path.exists():
            return {"error": f"Test executable not found: {exe_name}.exe. Build with BUILD_{suite.upper()}_TESTS=ON"}

        result = _run([str(exe_path)], timeout=60)

        # Parse Catch2-style output
        output = result.get("stdout", "") + result.get("stderr", "")
        passed = 0
        failed = 0
        for line in output.split("\n"):
            if "test cases" in line.lower() and "passed" in line.lower():
                # "All 59 test cases passed"
                import re
                m = re.search(r"(\d+)\s+test\s+cases?\s+passed", line, re.I)
                if m:
                    passed = int(m.group(1))
            elif "failed" in line.lower() and "test" in line.lower():
                import re
                m = re.search(r"(\d+)\s+(?:test|assertion).*failed", line, re.I)
                if m:
                    failed = int(m.group(1))

        return {
            "suite": suite,
            "passed": result["success"],
            "tests_passed": passed,
            "tests_failed": failed,
            "output": output[-3000:],
        }

    @mcp.tool
    def test_validate_shader_spv(spv_path: str) -> dict:
        """Validate a compiled SPIR-V binary using spirv-val.

        spv_path: path to .spv file (relative to project root)"""
        spirv_val = shutil.which("spirv-val")
        if not spirv_val:
            return {"error": "spirv-val not found. Install Vulkan SDK."}

        full_path = PROJECT_ROOT / spv_path
        if not full_path.exists():
            return {"error": f"SPV file not found: {spv_path}"}

        result = _run([spirv_val, str(full_path)])
        return {
            "spv": spv_path,
            "valid": result["success"],
            "errors": result.get("stderr", ""),
        }
