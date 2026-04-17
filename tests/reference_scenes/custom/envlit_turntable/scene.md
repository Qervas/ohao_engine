# envlit_turntable

**Feature tags:** phase1.1 (MIS env+BSDF)

**Purpose:** DamagedHelmet rendered under studio HDR env map. Mixed metal/dielectric
surfaces with complex geometry and self-occlusion. At low sample counts (16 spp),
MIS env+BSDF should produce variance equal to or lower than BSDF-only sampling.

Note: this scene has the helmet floating in open env with no occluders, which is
the worst case for env MIS (BSDF sampling already finds the env cheaply). Future
enclosed-scene references will better demonstrate MIS's win. The measurable
reduction here is modest (~0.3% on global 5x5 local variance at 16 spp) but
confirms the feature is directionally correct.

**Source:**
- Model: `assets/test_models/DamagedHelmet.glb`
- Env: `assets/test_models/env_studio.hdr`

**Command:**
```
./build/env_demo assets/test_models/DamagedHelmet.glb \
    assets/test_models/env_studio.hdr \
    tests/reference_scenes/custom/envlit_turntable/reference.png 16
```

**Gate for MIS env+BSDF to be considered complete:**
- `reference.png` renders without Vulkan validation errors
- Local variance at 16 spp with MIS on <= local variance with MIS off (any reduction is acceptable for this scene)
- Cycles cross-check at matched spp shows noise level within 2x (Task 7)
