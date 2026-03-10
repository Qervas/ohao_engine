"""Code generation — create render passes, shaders, and endpoints from templates.

Uses Jinja2 templates derived from existing codebase patterns. Generated code
matches the project's conventions for immediate integration.
"""

from pathlib import Path

try:
    from jinja2 import Environment, FileSystemLoader
    HAS_JINJA = True
except ImportError:
    HAS_JINJA = False

PROJECT_ROOT = Path(__file__).parent.parent
TEMPLATE_DIR = Path(__file__).parent / "templates"

# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def _get_env() -> "Environment":
    """Get Jinja2 template environment."""
    if not HAS_JINJA:
        raise RuntimeError("Jinja2 not installed. Run: pip install Jinja2")
    return Environment(
        loader=FileSystemLoader(str(TEMPLATE_DIR)),
        keep_trailing_newline=True,
        trim_blocks=True,
        lstrip_blocks=True,
    )


def _to_class_name(name: str) -> str:
    """Convert 'my_effect' to 'MyEffectPass'."""
    parts = name.replace("-", "_").split("_")
    return "".join(p.capitalize() for p in parts) + "Pass"


def _to_snake(name: str) -> str:
    """Ensure snake_case."""
    return name.replace("-", "_").lower()


def _to_shader_name(name: str, shader_type: str) -> str:
    """Convert 'my_effect' to 'compute_my_effect.comp'."""
    snake = _to_snake(name)
    if shader_type == "compute":
        return f"compute/{snake}.comp"
    return f"{snake}.{shader_type}"


# ─────────────────────────────────────────────────────────────────────────────
# Registration
# ─────────────────────────────────────────────────────────────────────────────

def register(mcp, bridge):
    """Register code generation tools on the MCP server."""

    @mcp.tool
    def generate_compute_pass(
        name: str,
        description: str,
        bindings: list[dict] | None = None,
        push_constants: list[dict] | None = None,
        output_format: str = "VK_FORMAT_R16G16B16A16_SFLOAT",
        write_files: bool = False,
    ) -> dict:
        """Generate a new compute render pass (C++ header, source, GLSL shader).

        name: pass name in snake_case (e.g., 'ambient_occlusion')
        description: what the pass does
        bindings: list of {name, type, stage} dicts. type: 'image', 'buffer', 'sampler'
        push_constants: list of {name, type} dicts. type: 'mat4', 'vec4', 'float', 'int'
        output_format: Vulkan format for output image
        write_files: if True, writes files to disk. If False, returns content for review.

        Example:
            generate_compute_pass(
                name='light_scatter',
                description='Screen-space light scattering',
                bindings=[
                    {'name': 'depth', 'type': 'image'},
                    {'name': 'hdr_input', 'type': 'image'},
                ],
                push_constants=[
                    {'name': 'invViewProj', 'type': 'mat4'},
                    {'name': 'lightDir', 'type': 'vec4'},
                ],
            )"""
        env = _get_env()
        class_name = _to_class_name(name)
        snake = _to_snake(name)

        ctx = {
            "name": snake,
            "class_name": class_name,
            "description": description,
            "bindings": bindings or [
                {"name": "output", "type": "image"},
                {"name": "depth", "type": "image"},
            ],
            "push_constants": push_constants or [],
            "output_format": output_format,
            "shader_spv": f"compute_{snake}.comp.spv",
        }

        files = {}
        for template_name, output_path in [
            ("compute_pass.hpp.j2", f"src/renderer/passes/{snake}_pass.hpp"),
            ("compute_pass.cpp.j2", f"src/renderer/passes/{snake}_pass.cpp"),
            ("compute_shader.comp.j2", f"shaders/compute/{snake}.comp"),
        ]:
            tmpl = env.get_template(template_name)
            files[output_path] = tmpl.render(**ctx)

        if write_files:
            for path, content in files.items():
                full = PROJECT_ROOT / path
                full.parent.mkdir(parents=True, exist_ok=True)
                full.write_text(content, encoding="utf-8")

        return {
            "generated": list(files.keys()),
            "class_name": class_name,
            "shader_spv": ctx["shader_spv"],
            "written_to_disk": write_files,
            "files": {k: v[:500] + "..." if len(v) > 500 else v for k, v in files.items()},
        }

    @mcp.tool
    def generate_shader_include(
        name: str,
        description: str,
        functions: list[dict] | None = None,
        write_files: bool = False,
    ) -> dict:
        """Generate a shared GLSL include file.

        name: include name in snake_case (e.g., 'noise_functions')
        description: what this include provides
        functions: list of {name, return_type, params, body} dicts
        write_files: write to disk or return for review

        Example:
            generate_shader_include(
                name='noise',
                description='Noise generation functions',
                functions=[{
                    'name': 'hash31',
                    'return_type': 'float',
                    'params': 'vec3 p',
                    'body': 'return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);'
                }],
            )"""
        env = _get_env()
        snake = _to_snake(name)

        ctx = {
            "name": snake,
            "guard": snake.upper(),
            "description": description,
            "functions": functions or [],
        }

        tmpl = env.get_template("shader_include.glsl.j2")
        content = tmpl.render(**ctx)
        output_path = f"shaders/includes/common/{snake}.glsl"

        if write_files:
            full = PROJECT_ROOT / output_path
            full.parent.mkdir(parents=True, exist_ok=True)
            full.write_text(content, encoding="utf-8")

        return {
            "generated": output_path,
            "written_to_disk": write_files,
            "content": content,
        }

    @mcp.tool
    def generate_integration_patch(
        pass_name: str,
        pass_order: float = 4.8,
        pass_type: str = "compute",
        dependencies: list[str] | None = None,
    ) -> dict:
        """Generate the exact code needed to integrate a new render pass into
        the deferred renderer. Returns the patches for deferred_renderer.hpp and .cpp.

        pass_name: snake_case name (e.g., 'light_scatter')
        pass_order: execution order (2.0=GBuffer, 3.0=SSAO, 4.0=Lighting, 5.0=Post)
        pass_type: 'compute' or 'graphics'
        dependencies: list of passes this depends on (e.g., ['lighting', 'depth'])"""
        class_name = _to_class_name(pass_name)
        snake = _to_snake(pass_name)
        member_name = f"m_{snake}Pass"

        deps = dependencies or []
        dep_setters = []
        for dep in deps:
            if dep == "depth":
                dep_setters.append(f'{member_name}->setDepthBuffer(m_gbufferPass->getDepthView(), VK_NULL_HANDLE);')
            elif dep == "lighting":
                dep_setters.append(f'{member_name}->setHDROutput(m_lightingPass->getOutputView(), m_lightingPass->getOutputImage());')
            elif dep == "normal":
                dep_setters.append(f'{member_name}->setNormalBuffer(m_gbufferPass->getNormalView());')

        header_patch = f"""
// In deferred_renderer.hpp — add to includes:
#include "renderer/passes/{snake}_pass.hpp"

// In deferred_renderer.hpp — add to member variables (near line 359):
std::unique_ptr<{class_name}> {member_name};"""

        init_patch = f"""
// In deferred_renderer.cpp::initialize() — add after other pass inits:
{member_name} = std::make_unique<{class_name}>();
if (!{member_name}->initialize(device, physicalDevice)) {{
    std::cerr << "DeferredRenderer: {class_name} failed (non-fatal)" << std::endl;
    {member_name}.reset();
}} else {{
    {chr(10).join('    ' + s for s in dep_setters)}
    std::cout << "DeferredRenderer: {class_name} OK" << std::endl;
}}"""

        if pass_type == "compute":
            render_patch = f"""
// In deferred_renderer.cpp::render() — add at step {pass_order}:
if ({member_name}) {{
    {member_name}->execute(cmd, m_currentFrame);
}}"""
        else:
            render_patch = f"""
// In deferred_renderer.cpp::render() — add at step {pass_order}:
if ({member_name}) {{
    {member_name}->setViewProjection(m_viewMatrix, m_projMatrix);
    {member_name}->execute(cmd, m_currentFrame);
}}"""

        cleanup_patch = f"""
// In deferred_renderer.cpp::cleanup():
if ({member_name}) {member_name}->cleanup();"""

        return {
            "pass_name": pass_name,
            "class_name": class_name,
            "member_name": member_name,
            "patches": {
                "header_include": f'#include "renderer/passes/{snake}_pass.hpp"',
                "header_member": f"std::unique_ptr<{class_name}> {member_name};",
                "initialize": init_patch,
                "render": render_patch,
                "cleanup": cleanup_patch,
            },
        }

    @mcp.tool
    def generate_list_templates() -> dict:
        """List all available code generation templates with descriptions."""
        templates = []
        if TEMPLATE_DIR.exists():
            for f in sorted(TEMPLATE_DIR.glob("*.j2")):
                # Read first line for description
                first_line = ""
                try:
                    first_line = f.read_text(encoding="utf-8").split("\n")[0]
                    if first_line.startswith("{#"):
                        first_line = first_line.strip("{# }")
                except Exception:
                    pass
                templates.append({"name": f.name, "description": first_line})

        return {
            "template_count": len(templates),
            "templates": templates,
            "template_dir": str(TEMPLATE_DIR),
        }
