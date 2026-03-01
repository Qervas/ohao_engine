# OHAO API Keyword Index
# Format:  keyword, keyword2 → file.md#section
# Usage:   Grep("your_keyword", "docs/INDEX.md") → read that file#section

# === PHYSICS — Bodies ===
body, rigid body, physics body, dynamic, static, kinematic, mass, friction, restitution → physics.md#bodies
apply_force, apply_impulse, apply_torque, velocity, linear_velocity, angular_velocity → physics.md#bodies
gravity, gravity_scale, damping, linear_damping, angular_damping → physics.md#bodies
CCD, continuous collision, tunneling, fast moving → physics.md#bodies
collision layer, LAYER_, body layer, setBodyLayer → physics.md#layers

# === PHYSICS — Constraints / Joints ===
constraint, joint, hinge, slider, fixed, point, distance, cone, six_dof, SixDOF → physics.md#constraints
create_constraint, destroy_constraint, set_constraint → physics.md#constraints
motor, constraint_motor, set_constraint_motor, motor_speed → physics.md#constraints
limits, constraint_limits, set_constraint_limits → physics.md#constraints
breaking, breakable, constraint_breaking, set_constraint_breaking, broken → physics.md#breaking
get_and_clear_broken_constraints, broken_constraints → physics.md#breaking
ragdoll, chain, rope, pendulum, door, vehicle joint → physics.md#constraints

# === PHYSICS — Grab / Throw ===
grab, throw, drag, mouse spring, pick up, fling → physics.md#grab
grab_body, move_grab, release_grab, throw_grab, token → physics.md#grab

# === PHYSICS — Queries / Raycasting ===
raycast, cast_ray, ray, raycasting → physics.md#queries
overlap, overlap_sphere, overlap_box → physics.md#queries
cast_ray_all, all hits, multi-hit → physics.md#queries
layer_mask, filter, collision filter → physics.md#queries

# === PHYSICS — Character Controller ===
character, capsule, player physics, slope, ground state → physics.md#character
create_character, destroy_character, get_character_state → physics.md#character
set_character_position, set_character_velocity, update_character → physics.md#character
ON_GROUND, ON_STEEP, NOT_SUPPORTED, IN_AIR, GROUND_ → physics.md#character

# === PHYSICS — Simulation ===
play physics, pause physics, step physics, stop physics, physics speed → physics.md#simulation
play_physics, pause_physics, step_physics, stop_physics, set_physics_speed → physics.md#simulation
contact, collision event, contact callback, onContactBegin → physics.md#contacts

# === RENDERING — Post-Processing Effects ===
bloom, threshold, intensity → render.md#effects
ssao, ambient occlusion, radius, ssao_intensity → render.md#effects
ssgi, global illumination, indirect, ssgi_radius → render.md#effects
ssr, screen space reflections, reflection → render.md#effects
volumetric, fog, density, scattering, volumetrics → render.md#effects
motion blur, blur, motion_blur_intensity → render.md#effects
dof, depth of field, focus, aperture, bokeh → render.md#effects
taa, temporal antialiasing, blend_factor → render.md#effects
tonemapping, tonemap, ACES, Reinhard, Uncharted, exposure, gamma → render.md#effects
OhaoSettings, apply_effect, recommended_for, list_stable, disable_effect → render.md#settings-api

# === RENDERING — Materials ===
material, texture, albedo, normal map, pbr, metallic, roughness → render.md#materials
set_actor_texture, set_actor_normal_map, set_actor_pbr → render.md#materials
material_preset, preset, set_actor_material_preset → render.md#materials
ai material, ai texture, pollinations, OhaoAI, apply_ai_material → render.md#ai-materials

# === RENDERING — Viewport ===
wireframe, grid, toggle, enable, disable → render.md#viewport
get_render_stats, stats, fps, draw calls → render.md#viewport
set_exposure, set_gamma, set_tonemap_operator → render.md#viewport

# === SCENE — Building ===
scene build, build scene, Ohao.scene, OhaoSceneBuilder → scene.md#builder
template, arena, corridor, outdoor → scene.md#builder
rendering preset, horror, cyberpunk, bright, cinematic, fps_action, minimal → scene.md#builder
add_cube, add_sphere, add_plane, add_cylinder → scene.md#actors
add_directional_light, add_point_light → scene.md#lights
finish_sync, sync, clear_scene → scene.md#sync

# === SCENE — Actors ===
actor, mesh, remove_actor, has_actor → scene.md#actors
set_actor_position, set_actor_rotation, set_actor_scale → scene.md#actors
OhaoMeshInstance, self-managing, visual actor → scene.md#mesh-instance
import_model, model, GLTF, OBJ → scene.md#actors

# === SCENE — Camera ===
camera, FPS camera, orbit camera, camera mode → scene.md#camera
set_camera_mode, CAMERA_FPS, CAMERA_ORBIT → scene.md#camera
set_camera_position, get_camera_position, focus_on_scene → scene.md#camera
mouse_sensitivity, move_speed, camera_rotation → scene.md#camera

# === AUDIO ===
play sound, audio, sfx, music, ambient → audio.md
play_sound, play_sound_at, stop_sound, pause_sound, resume_sound → audio.md
volume, set_sound_volume, set_category_volume, set_master_volume → audio.md
3D audio, spatial, positional, play_sfx_at, play_ambient_at → audio.md
SFX, AUDIO_SFX, AUDIO_MUSIC, AUDIO_AMBIENT → audio.md
handle, sound_handle, stop_all_sounds → audio.md

# === GDSCRIPT — Autoload & Helpers ===
Ohao, autoload, singleton, Ohao.viewport, Ohao.scene → gdscript.md#autoload
OhaoConst, constants, BODY_, SHAPE_, CAMERA_, LAYER_ → gdscript.md#constants
OhaoPhysicsHelpers, player_body, make_physics_body → gdscript.md#physics-body
make_mesh, Ohao.make_mesh, OhaoMeshInstance factory → gdscript.md#mesh-helpers
settings panel, F2, create_settings_panel, in-game settings → gdscript.md#settings-panel

# === GDSCRIPT — Patterns ===
FPS player, player controller, first person → gdscript.md#fps-pattern
input mode, game mode, editor mode, INPUT_GAME, INPUT_EDITOR → gdscript.md#input-mode
particles, spawn_particles, muzzle flash, explosion, smoke → gdscript.md#particles
selection, pick_object_at, selected_actor → gdscript.md#selection
