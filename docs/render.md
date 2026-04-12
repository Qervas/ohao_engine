# OHAO Rendering API

Parent: docs/API.md | Search: docs/INDEX.md

---

## #effects

All effects are disabled by default. Enable individually or use OhaoSettings presets.

### Toggles
```gdscript
vp.set_bloom_enabled(true)
vp.set_ssao_enabled(true)
vp.set_ssgi_enabled(true)       # experimental — color bleeding
vp.set_ssr_enabled(true)        # experimental — screen-space reflections
vp.set_volumetrics_enabled(true)
vp.set_motion_blur_enabled(true)
vp.set_dof_enabled(true)
vp.set_taa_enabled(true)
vp.set_tonemapping_enabled(true)
vp.set_wireframe_enabled(false)
vp.set_grid_enabled(false)
```

### Parameters
```gdscript
# Bloom
vp.set_bloom_threshold(0.8)     # HDR value above which light blooms (lower = more bloom)
vp.set_bloom_intensity(1.0)

# SSAO
vp.set_ssao_radius(0.5)         # world-space sampling radius
vp.set_ssao_intensity(1.0)

# SSGI (experimental)
vp.set_ssgi_radius(1.0)
vp.set_ssgi_intensity(0.5)
vp.set_ssgi_sample_count(16)    # higher = better quality, more GPU

# SSR (experimental)
vp.set_ssr_max_distance(10.0)
vp.set_ssr_thickness(0.1)

# Volumetrics / Fog
vp.set_volumetric_density(0.02)
vp.set_volumetric_scattering(0.5)
vp.set_fog_color(Color(0.5, 0.6, 0.7))

# Motion Blur
vp.set_motion_blur_intensity(0.5)
vp.set_motion_blur_samples(8)

# Depth of Field
vp.set_dof_focus_distance(5.0)
vp.set_dof_aperture(2.8)
vp.set_dof_max_blur(0.02)

# TAA
vp.set_taa_blend_factor(0.1)    # lower = more ghosting, more stable

# Tonemapping
vp.set_tonemap_operator(OhaoConst.TONEMAP_ACES)
vp.set_exposure(1.0)
vp.set_gamma(2.2)
# TONEMAP_ACES=0  TONEMAP_REINHARD=1  TONEMAP_UNCHARTED2=2  TONEMAP_NEUTRAL=3
```

### Named rendering presets (use in scene builder "rendering" key)
| Preset | Effects enabled |
|--------|----------------|
| `horror` | bloom(low), ssao(high), volumetrics, tonemapping |
| `cyberpunk` | bloom(high), ssao, tonemapping(ACES), exposure++ |
| `bright` | tonemapping only, high exposure |
| `cinematic` | bloom, ssao, ssgi, dof, taa, tonemapping |
| `fps_action` | bloom, motion_blur, taa, tonemapping |
| `minimal` | tonemapping only |

---

## #settings-api

OhaoSettings provides a discoverable, dict-based API for AI agents and runtime UIs.

```gdscript
var s = Ohao.settings()

# Apply/disable an effect
s.apply_effect(vp, "bloom", {"bloom_threshold": 0.4, "bloom_intensity": 1.2})
s.disable_effect(vp, "bloom")

# Discover
s.list_all()            # → ["bloom", "ssao", "ssgi", "ssr", "taa",
                        #    "volumetrics", "motion_blur", "dof", "tonemapping"]
s.list_stable()         # → ["bloom", "ssao", "tonemapping", "taa", "volumetrics"]
s.recommended_for(["cyberpunk"])   # → ["bloom", "ssao", "tonemapping"]
s.get_effect_status(vp, "bloom")  # → {"enabled": true, "bloom_threshold": 0.4, ...}
s.get_effect_info("bloom")        # → full metadata dict

# Stable: bloom, ssao, tonemapping, taa, volumetrics
# Experimental: ssgi, ssr, motion_blur, dof
```

---

## #materials

```gdscript
# Texture (file path, relative or absolute)
vp.set_actor_texture("Box", "res://textures/wood.png")
vp.set_actor_normal_map("Box", "res://textures/wood_normal.png")

# PBR parameters
vp.set_actor_pbr("Box", 0.0, 0.8)  # metallic, roughness

# Named presets
vp.set_actor_material_preset("Box", "metal")
vp.set_actor_material_preset("Box", "stone")
# Available presets: see OhaoPresets.MATERIALS in gdscript.md
```

---

## #ai-materials

AI-generated textures via Pollinations.ai (free, ~5000/day).

```gdscript
# One-liner — generates texture from text description
await OhaoAI.apply_ai_material(vp, "Box", "worn stone with moss")

# In scene builder dict
Ohao.scene().build(vp, {
    "objects": [
        {"type": "cube", "name": "Wall", "material": "ai:cracked concrete"},
    ]
})
```

Requires `.env` with `POLLINATIONS_API_KEY=sk_...` at repo root.

---

## #viewport

```gdscript
# Stats
var stats = vp.get_render_stats()
# stats.fps, stats.draw_calls, stats.triangle_count, stats.memory_mb

# Selection (editor mode)
var name = vp.pick_object_at(screen_pos)   # Vector2 screen pos
var name = vp.get_selected_actor_name()
```
