# RT GI's bounce is shadowed

**Spec:** none (a defect found diagnosing `check_fireflies`, `docs/changes/2026-10-29-hdri-orientation.md` section 7).
**Status:** fixed. Budgets moved by Frank's ruling (2026-10-29): `cornell_720p` `rt.gi` 0.10 → 0.11 ms, `play_hud_1080p` frame
2.39 → 2.45 ms. Goldens 32 of 32, every deferred one moved and the large movers looked at. Gate 50 of 50.

## 1. Summary

A GI ray that hits a surface brings back the light that surface reflects: its albedo over π times the direct light it receives
(`gi_trace.slang`, `directAt`). That direct light had no shadow ray, so a hit in shadow bounced full light. Every surface whose GI
rays land on shadowed ground (an underside, a rim, the floor under an object) received light that was not there. The persistent
bright specks `check_fireflies` found on thin features after the HDRI fix were this: the enemy helmet's lower lip, a glove's
underside, the gun's edges.

Now the bounce point is shadowed with one ray: of the up to four lights GI sees, one is chosen with probability proportional to
the luminance it would give unshadowed, a shadow ray goes to it (alpha-tested, as the main trace tests candidates), and its light
is divided by that probability. The mean is every light's shadowed light; the cost is one ray per GI ray, not four.

## 2. How it was found

Each step measured on the play scene from the spawn looking west (`check_fireflies`' view), counting GI specks (raised over the
difference's 5×5 median and over the GI-on image's own, 12 levels each):

| Capture | Specks | On edges |
|---|---|---|
| TAA, 1 / 4 / 32 frames | 262 / 32 / 6 | 243 / 31 / 6 |
| no post, 1 / 4 / 32 frames | 250 / 137 / 122 | 236 / 136 / 121 |

Without TAA about 120 specks stay on edges with a converged history: a wrong value, not noise. Then, one variable at a time
(no post, 32 frames / TAA, 4 frames):

| Experiment | Specks | Kept |
|---|---|---|
| baseline, half resolution | 122 / 32 | |
| RT GI at full resolution | 131 / 35 | no: the half-resolution path is not the cause |
| ray origin 5 mm off the surface (from 5 cm) | 127 / 34 | no |
| a back-face hit brings no light | 115 / 26 | no: a small effect, and scene materials default to double-sided |
| a shadow ray to every light at the hit | **70 / 12** | the cause; 4 rays a GI ray |
| one shadow ray, light chosen by contribution | **70 / 14** | **yes** |

The remaining specks are dimmer: the helmet lip went from (139, 108, 101) to (112, 91, 102) against (77, 75, 92) without GI.
Whether what is left is right needs the path tracer as a referee (below).

## 3. Results

**Cost** (`render_bench --only`, GPU timestamps; the runs were flagged noisy on the CPU side):

| | Before | Every light | One sampled light | Budget |
|---|---|---|---|---|
| `cornell_720p` `rt.gi` | 0.071 | 0.133 | 0.101 | 0.10 → **0.11** |
| `play_hud_1080p` `rt.gi` | 0.275 | 0.467 | 0.385 | |
| `play_hud_1080p` frame | 2.30–2.35 | 2.51 | 2.433 | 2.39 → **2.45** |

**Goldens:** every deferred scene moved (RT GI is in every one); every path-traced scene stayed exact. Looked at: the helmet (the
visor and the shell's top darker, as the path tracer has them; its mean distance to the path-traced golden 31.43 → 31.27, blurred,
640×360), Sponza (max_abs 194: the arches' undersides and the upper gallery darker, no longer lit by walls their light does not
reach), the metal spheres, Cornell (max_abs 3).

**Gate:** 50 of 50 (`python tools/check.py`, labels quick, gpu, docs). `check_fireflies` **14** (32 before this step).
`t34_doorway_dims_gi` still passes: 10.49 with the cloud against 16.42 without, 36.1% darker (bar 20%); its cloudless reading was
10.64 earlier the same day, before both the HDRI fix and this one, so the change is not attributed to either alone.

## 4. Found on the way, not changed

**Deferred and the path tracer disagree far more than this fix moves them.** Cornell's deferred frame has a mean luma of 64.5
against its path-traced golden's 120.4, about half as bright; the helmet room is closer (114 against 108). The path tracer is the
reference, so this is the next step: compare every scene both ways and explain the gap, so that GI changes are judged by a number.

## 5. Docs

The trace's header (`gi_trace.slang`); the monograph's `hybrid/gi-technique` (the shadowed bounce replaces "no shadow ray is
traced") and `shaders/rt-shaders`.
