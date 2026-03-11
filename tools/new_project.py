"""Create a new isolated Godot project that uses the OHAO Engine.

Usage:
    python tools/new_project.py <name> [location]

Examples:
    python tools/new_project.py horror_mansion           # → ../horror_mansion/
    python tools/new_project.py my_game C:/Games/my_game # → C:/Games/my_game/

The engine DLL and addons are symlinked (or copied if symlinks fail).
Game code lives entirely outside the engine repo.
"""

import os
import shutil
import sys
from pathlib import Path

ENGINE_ROOT = Path(__file__).parent.parent
GODOT_PROJECT = ENGINE_ROOT / "godot_editor" / "project"

# What we need from the engine
DLL_NAME = "libohao.windows.template_debug.x86_64.dll"
GDEXT_FILE = "ohao.gdextension"
ADDON_DIRS = ["ohao_helpers", "ohao"]


def create_project(name: str, location: str | None = None) -> Path:
    """Create a new OHAO project at the given location."""
    if location:
        project_dir = Path(location)
    else:
        project_dir = ENGINE_ROOT.parent / name

    if project_dir.exists():
        print(f"ERROR: Directory already exists: {project_dir}")
        sys.exit(1)

    print(f"Creating project: {project_dir}")

    # Create directory structure
    for d in ["scripts", "scenes", "assets", "bin", "addons"]:
        (project_dir / d).mkdir(parents=True, exist_ok=True)

    # 1. Symlink or copy DLL
    dll_src = GODOT_PROJECT / "bin" / DLL_NAME
    dll_dst = project_dir / "bin" / DLL_NAME
    _link_or_copy(dll_src, dll_dst)

    # 2. Copy .gdextension (small file, always copy)
    gdext_src = GODOT_PROJECT / "bin" / GDEXT_FILE
    gdext_dst = project_dir / "bin" / GDEXT_FILE
    if gdext_src.exists():
        shutil.copy2(gdext_src, gdext_dst)
        print(f"  Copied: {GDEXT_FILE}")

    # 3. Symlink or copy addons
    for addon in ADDON_DIRS:
        addon_src = GODOT_PROJECT / "addons" / addon
        addon_dst = project_dir / "addons" / addon
        _link_or_copy_dir(addon_src, addon_dst)

    # 4. Write project.godot
    _write_project_godot(project_dir, name)

    # 5. Write main scene
    _write_main_scene(project_dir)

    # 6. Write main script
    _write_main_script(project_dir, name)

    # 7. Write .gitignore
    (project_dir / ".gitignore").write_text(
        "*.import\n.godot/\n*.uid\n*.tmp\n",
        encoding="utf-8",
    )

    print(f"\nProject created: {project_dir}")
    print(f"  cd \"{project_dir}\" && godot project.godot -e")
    return project_dir


def _link_or_copy(src: Path, dst: Path) -> None:
    """Try symlink, fall back to copy."""
    if not src.exists():
        print(f"  WARNING: Source not found: {src}")
        return
    try:
        os.symlink(src, dst)
        print(f"  Symlinked: {dst.name} -> {src}")
    except OSError:
        shutil.copy2(src, dst)
        print(f"  Copied: {dst.name} (symlink failed — run as admin for symlinks)")


def _link_or_copy_dir(src: Path, dst: Path) -> None:
    """Try directory symlink, fall back to copytree."""
    if not src.exists():
        print(f"  WARNING: Source not found: {src}")
        return
    try:
        os.symlink(src, dst, target_is_directory=True)
        print(f"  Symlinked: addons/{dst.name}/ -> {src}")
    except OSError:
        shutil.copytree(src, dst)
        print(f"  Copied: addons/{dst.name}/ (symlink failed)")


def _write_project_godot(project_dir: Path, name: str) -> None:
    """Write a minimal project.godot."""
    content = f"""; Engine configuration file.
; It's best edited using the editor UI and not directly,
; but you can add to it manually if you know what you're doing.

config_version=5

[application]

config/name="{name}"
run/main_scene="res://scenes/main.tscn"
config/features=PackedStringArray("4.3")

[autoload]

Ohao="*res://addons/ohao_helpers/ohao_helpers.gd"
OhaoServer="*res://addons/ohao_helpers/ohao_server.gd"

[editor_plugins]

enabled=PackedStringArray("res://addons/ohao/plugin.cfg")

[input]

move_forward={{
"deadzone": 0.5,
"events": [Object(InputEventKey,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"pressed":false,"keycode":87,"physical_keycode":0,"key_label":0,"unicode":119,"location":0,"echo":false,"script":null)]
}}
move_backward={{
"deadzone": 0.5,
"events": [Object(InputEventKey,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"pressed":false,"keycode":83,"physical_keycode":0,"key_label":0,"unicode":115,"location":0,"echo":false,"script":null)]
}}
move_left={{
"deadzone": 0.5,
"events": [Object(InputEventKey,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"pressed":false,"keycode":65,"physical_keycode":0,"key_label":0,"unicode":97,"location":0,"echo":false,"script":null)]
}}
move_right={{
"deadzone": 0.5,
"events": [Object(InputEventKey,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"pressed":false,"keycode":68,"physical_keycode":0,"key_label":0,"unicode":100,"location":0,"echo":false,"script":null)]
}}
jump={{
"deadzone": 0.5,
"events": [Object(InputEventKey,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"pressed":false,"keycode":32,"physical_keycode":0,"key_label":0,"unicode":32,"location":0,"echo":false,"script":null)]
}}
shoot={{
"deadzone": 0.5,
"events": [Object(InputEventMouseButton,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"button_mask":1,"position":Vector2(0,0),"global_position":Vector2(0,0),"factor":1.0,"button_index":1,"canceled":false,"pressed":false,"double_click":false,"script":null)]
}}
reload={{
"deadzone": 0.5,
"events": [Object(InputEventKey,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"pressed":false,"keycode":82,"physical_keycode":0,"key_label":0,"unicode":114,"location":0,"echo":false,"script":null)]
}}
interact={{
"deadzone": 0.5,
"events": [Object(InputEventKey,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"pressed":false,"keycode":69,"physical_keycode":0,"key_label":0,"unicode":101,"location":0,"echo":false,"script":null)]
}}
weapon_1={{
"deadzone": 0.5,
"events": [Object(InputEventKey,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"pressed":false,"keycode":49,"physical_keycode":0,"key_label":0,"unicode":49,"location":0,"echo":false,"script":null)]
}}
weapon_2={{
"deadzone": 0.5,
"events": [Object(InputEventKey,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"pressed":false,"keycode":50,"physical_keycode":0,"key_label":0,"unicode":50,"location":0,"echo":false,"script":null)]
}}
weapon_3={{
"deadzone": 0.5,
"events": [Object(InputEventKey,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"pressed":false,"keycode":51,"physical_keycode":0,"key_label":0,"unicode":51,"location":0,"echo":false,"script":null)]
}}
crouch={{
"deadzone": 0.5,
"events": [Object(InputEventKey,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"pressed":false,"keycode":4194326,"physical_keycode":0,"key_label":0,"unicode":0,"location":0,"echo":false,"script":null)]
}}
sprint={{
"deadzone": 0.5,
"events": [Object(InputEventKey,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"pressed":false,"keycode":4194325,"physical_keycode":0,"key_label":0,"unicode":0,"location":0,"echo":false,"script":null)]
}}
"""
    (project_dir / "project.godot").write_text(content, encoding="utf-8")
    print("  Wrote: project.godot")


def _write_main_scene(project_dir: Path) -> None:
    content = """[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://scripts/main.gd" id="1"]

[node name="Main" type="Control"]
layout_mode = 3
anchors_preset = 15
anchor_right = 1.0
anchor_bottom = 1.0
script = ExtResource("1")

[node name="OhaoViewport" type="OhaoViewport" parent="."]
layout_mode = 1
anchors_preset = 15
anchor_right = 1.0
anchor_bottom = 1.0
"""
    (project_dir / "scenes" / "main.tscn").write_text(content, encoding="utf-8")
    print("  Wrote: scenes/main.tscn")


def _write_main_script(project_dir: Path, name: str) -> None:
    content = f'''extends Control
## {name} — built with OHAO Engine

@onready var ohao_viewport: OhaoViewport = $OhaoViewport

func _ready() -> void:
\tif not ohao_viewport:
\t\tpush_error("OhaoViewport not found")
\t\treturn
\tcall_deferred("_setup")

func _setup() -> void:
\t# Build your game here — use /new-game, /new-scene, /atmosphere, etc.
\tohao_viewport.add_cube("HelloWorld", Vector3(0, 1, 0), Vector3.ZERO,
\t\tVector3(2, 2, 2), Color(0.2, 0.6, 1.0))
\tohao_viewport.add_directional_light("Sun", Vector3(5, 10, 5),
\t\tVector3(-0.5, -1, -0.3).normalized(), Color.WHITE, 2.0)
\tohao_viewport.finish_sync()
\tohao_viewport.focus_on_scene()
\tprint("[{name}] Ready")
'''
    (project_dir / "scripts" / "main.gd").write_text(content, encoding="utf-8")
    print("  Wrote: scripts/main.gd")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python tools/new_project.py <name> [location]")
        print("  Creates an isolated Godot project using OHAO Engine.")
        sys.exit(1)

    proj_name = sys.argv[1]
    proj_location = sys.argv[2] if len(sys.argv) > 2 else None
    create_project(proj_name, proj_location)
