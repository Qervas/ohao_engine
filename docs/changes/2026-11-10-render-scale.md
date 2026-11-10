# The render scale: TAA's resolve as the upscaler, and the window's real size

**Spec:** none of its own. `docs/ROADMAP.md` row 16 ("Render to budget": upscaling when a frame asks) and
`docs/specs/2026-10-27-feel-design.md`, "Performance budgets" (look latency). **Status:** built 2026-11-10, on
`step3-engine-game-boundary` after step 3's merge; local commits, not pushed.

## 1. Summary

Step 3 closed with `feel_bench` reading 67 ms of look latency on the 4060 laptop against a budget of 15. Measured
down, that was three things:
- the window drew 2.25 times the pixels asked for (the desktop's 150% scale);
- the bench counted a dead player's mouse movements, which no frame shows;
- the rest is the frame's GPU time plus the compositor's.

The window now draws the pixels asked for, the bench counts what is shown, and the deferred frame can be rendered
smaller and upscaled by TAA's resolve (`render.settings renderScale`). The new resolve is also sharper at scale 1
than the one it replaces, so it is the only one. Latency is 20.8 ms at scale 1, 16.7 ms at 0.667 and 12.7 ms at 0.5.

## 2. What was built

- **A window's size is pixels** (`85370acc`): `runtime/window.cpp` sizes the window so that its framebuffer is the
  size asked for, and again when Wayland reports the display's scale.
- **`feel_bench` refuses a sleeping display** (`56ce3f70`): exit 2 when no connected display is on.
- **A look nothing shows is not a sample** (`0ae2edb3`): `PhysicsScene::ignoresLook`.
- **The render scale** (this record's commit):
  - `DeferredRenderer` has a render size (the scene passes) and an output size (TAA's history, the display pass,
    the gizmo, the HUD). `setRenderScale`, 0.5 to 1.
  - `taa_resolve.slang` takes each frame's nearest jittered sample by its distance to the output pixel's centre
    (Karis 2014; Unreal's TAAU), and reprojects the history by the motion vectors less the frame's jitter.
  - The jitter is +-1/2 of a rendered pixel (it was +-1/4), 16 frames at scale 1 and 32 when upscaling.
  - `render.settings renderScale`; `feel_bench --render-scale S`.
- **Taken from the state of the art, and not:** the temporal upsample of Unreal's TAAU, which FSR 2 and DLSS SR
  build on. Not taken: a vendor SDK (a new dependency, Frank's to approve; `external/DLSS` is not on this machine),
  FSR 2's locks and reactive masks beyond the transparency mask TAA already had, a texture mip bias.

## 3. Bugs found and fixed

- **The play window drew 2880x1620 for 1920x1080.** GLFW sizes a Wayland window in the desktop's units and scales
  the framebuffer. Headless, the play scene's GPU frame is 8.7 to 9.3 ms at 1920x1080 and 19.8 to 20.0 ms at
  2880x1620; the window's frame was 20.4 ms.
- **TAA blurred every frame.** The motion vectors carry the frame's jitter, and the resolve reprojected the history
  by them whole, so the history was resampled a jitter away each frame. Found when the upscaler without history
  clipping came out softer than a bilinear stretch; an A/B on taking the jitter out showed it.
- **`feel_bench` measured looks nothing showed** (a dead player's), at about 55 ms each.
- **My own:** several runs were made with the display asleep, where the compositor discards every frame and a
  present is confirmed only when the next frame replaces it. I read that as present wait answering a frame late and
  built a Wayland presentation-time path before finding the cause. With the display on the two agree to 0.3 ms; the
  path is deleted, and the bench now refuses.

## 4. Spec corrections

None. `docs/ROADMAP.md` row 16 named FSR as the first upscaler; the engine's own resolve was built instead, with
no dependency.

## 5. Results

4060 laptop, the display on for every window run.

- **Quality, against a supersampled frame** (3840x2160 box-filtered to 1920x1080, 64 frames, a still camera; PSNR,
  dB). The old resolve against its own supersampled frame, ray tracing off: the helmet 38.1, Cornell 42.9, the play
  scene 48.9. The new resolve at scale 1 against the same frames: 42.1, 48.5, 51.5.

  | The new resolve, against its own supersampled frame | Scale 1 | 0.667 | 0.667, stretched bilinearly | 0.5 | 0.5, stretched |
  |---|---|---|---|---|---|
  | The play scene, ray tracing on | 46.3 | 42.3 | 41.3 | 40.3 | 39.5 |
  | The helmet, ray tracing off | 40.4 | 36.8 | 35.0 | | |
  | Cornell, ray tracing off | 47.5 | 43.0 | 42.7 | | |

- **Cost** (the play scene at 1920x1080, headless, GPU ms): 8.7 at scale 1, 4.7 at 0.667, 3.2 at 0.5; at 2560x1600,
  17.4 at scale 1. `render_bench play_hud_1080p`, three runs each on the commit before and on this one: 8.53 to 9.16
  and 8.71 to 9.28 for the frame, 0.253 to 0.267 and 0.266 to 0.281 for `post.taa`.
- **Look latency** (`feel_bench`, immediate, medians):

  | | Latency | p95 | Frame |
  |---|---|---|---|
  | Step 3's close (2880x1620 drawn, dead looks counted) | 67.4 ms | 103 ms | 20.4 ms |
  | The window's size fixed | 23 to 24 ms | 63 ms | 9.1 to 9.3 ms |
  | Dead looks not counted | 20.8 ms | 25.0 ms | 9.1 ms |
  | Render scale 0.667 | 16.7 ms | 17.9 ms | 4.7 ms |
  | Render scale 0.5 | 12.7 ms | 16.8 ms | 4.2 ms |

- **Gates:** 6 of 6. Three goldens moved with the resolve (the helmet, the play scene, glass: the ones rendered
  with TAA) and were updated after looking at each crop: sharper edges and detail, no other difference seen.
- **Validation:** zero messages over the play scene, the helmet and the smoke doorway at scales 0.5, 0.667, 0.75
  and 1, at four sizes.
- **Planted:** none for the upscaler (no gate was added). The display guard was checked both ways.

## 6. Known gaps

- **The 15 ms budget** is met at scale 0.5 only. From submit to present is 12 to 16 ms with a 3 to 5 ms GPU frame:
  the compositor's share (the panel is on the Intel GPU, KWin composites). A fullscreen window was not measured.
- **`frame_p99_over_median`** read 1.25 to 1.87 at scale 1 across the day's runs and 2.6 to 2.8 at scales 0.667
  and 0.5 (budget 1.5): frames alternate between about 4 and 12 ms under the one-frame-ahead wait. Not looked into.
- **A moving camera** was looked at in one window run (the player turning, a capture at each scale: no trails seen),
  not measured. No texture mip bias; the motion vector is the nearest sample's.
- **The default stays scale 1.** Which scale the game plays at is Frank's (quality against latency).
- **One NVIDIA Xid 13** ("SKEDCHECK16_CTA_THREAD_DIMENSION_ZERO", a compute dispatch with a zero dimension) was
  logged at 20:33:55 as a `feel_bench` window started, seconds after the display was woken. The run finished, the
  GPU works (`ohao doctor`), and nothing since has logged one. Not reproduced: headless frames down to 8x8 rendered
  pixels, under validation, log none. SSS's indirect dispatches run with x = 0 every frame in scenes without
  subsurface, without a fault.
- **The render budgets** were not checked (`render_bench --check`): they are the 5070's.
