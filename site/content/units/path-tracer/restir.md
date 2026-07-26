---
module: path-tracer
id: restir
title: ReSTIR GI reservoirs
standard: v2
figures: [path-tracer-restir-reservoir-planes]
---

## Keeping the sample instead of the average

A one-sample diffuse bounce does not fail as pixel grain. It *boils*: every frame
the cosine ray lands somewhere else, the wall's indirect term jumps, and
accumulation smears it into a crawling stain. Averaging cannot help — an averaged
colour is welded to the shading point that produced it.

ReSTIR (Ouyang et al. 2021) carries the **sample** instead: the vertex the bounce
ray hit, its normal, the radiance leaving it, and enough bookkeeping to re-weight
it for the surface the pixel is shading now. A sample can be re-evaluated at a new
shading point; a colour cannot. Hence six full-resolution images and a block of
the descriptor set.

## Twelve floats, three planes

Six fields: the secondary vertex `x_s`, its normal `n_s`, the outgoing radiance
`L_o` toward the primary hit, the RIS weight sum `wSum`, a confidence count `M`,
and the contribution weight `W`.

{{cite shaders/rt/pt_raygen_realtime.rgen "struct GIReservoir {"}}

Only five are persisted. Three RGBA32F planes give twelve floats: three `vec3`s
in RGB, and in the alphas `M`, `W`, and a validity flag set only where the
primary ray hit geometry. `wSum` is dropped — a stored reservoir re-enters the
stream with weight `p̂ · W · M`, reconstructible from the rest.

The planes are allocated twice — read as *prev*, written as *curr* — so a single
raygen dispatch can read-modify-write without aliasing itself:

{{cite ohao/render/rt/path_tracer_images.cpp "// --- ReSTIR GI reservoir ping-pong: 3 RGBA32F planes"}}

They occupy a contiguous six-binding block in the path tracer's set 0: 29–31
prev, 32–34 curr.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "for (uint32_t b = 29; b <= 34; ++b) {"}}

The price is unconditional: six `R32G32B32A32_SFLOAT` images at render resolution
— 96 bytes per pixel, roughly 190 MiB at 1920×1080.

{{figure path-tracer-restir-reservoir-planes "Conceptual layout — the alpha-channel packing of M, W and the validity flag, and the binding→image mapping across a ping-pong pair. Field names and binding numbers read from pt_raygen_realtime.rgen and path_tracer_descriptors.cpp; the byte figure is arithmetic, not a measurement."}}

## The weight that makes a reused sample legal

Resampled importance sampling draws $M$ cheap candidates $x_i$ from a source pdf
$p$, then keeps one survivor $y$ in proportion to a *target* $\hat p$ closer to
the integrand than $p$ is:

$$\langle I\rangle \;=\; \frac{f(y)}{\hat p(y)}\cdot\frac{1}{M}\sum_{i=1}^{M} w_i,
\qquad w_i=\frac{\hat p(x_i)}{p(x_i)}$$

$f$ is the diffuse GI at the primary hit $x_1$ — $(\rho/\pi)\,L_o\cos\theta$, for
$\rho$ the diffuse albedo (zero on metals) and $\theta$ the angle between the
normal and the direction to $x_s$; $p$ is the cosine-hemisphere pdf
$\cos\theta/\pi$ the bounce was drawn from; $\hat p$ is a scalar stand-in for
$f$, its luminance:

{{cite shaders/rt/pt_raygen_realtime.rgen "return giLum((albedoD / OHAO_PI) * Lo * cosT);"}}

Everything right of $f(y)$ collapses into the number stored as
$W = (\sum w_i) / (M\,\hat p(y))$, so shading is just $f(y)\cdot W$:

{{cite shaders/rt/pt_raygen_realtime.rgen "merged.W = (pHatHeld > 0.0 && merged.M > 0.0) ? merged.wSum / (merged.M * pHatHeld) : 0.0;"}}

`M` counts candidates, not frames: the N-samples-per-frame loop streams each
initial bounce into one reservoir before any reuse —

{{cite shaders/rt/pt_raygen_realtime.rgen "giReservoirUpdate(giCurr, xs, ns, Lo, w_i, rSel);"}}

— but only the bounces that *hit* something. That call is the sole `r.M += 1.0`
and sits in the else-branch; a cosine ray escaping to the environment goes
straight into the shaded result:

{{cite shaders/rt/pt_raygen_realtime.rgen "giEnvMissTotal += giAlbedoD * payload.color;"}}

So `M = N` in a closed box, and less under an HDRI.

## Re-evaluating the target is the entire correctness argument

The stored reservoir re-enters the stream with weight $\hat p'(y)\cdot W\cdot M$,
where $\hat p'$ is the target **re-computed at the current shading point** — not
the $\hat p$ in force when it was taken:

{{cite shaders/rt/pt_raygen_realtime.rgen "float pHatPrev = giTargetPHat(giAlbedoD, firstHitNormal,"}}

Two guards sit on top. The reprojected pixel must clear the geometry gate the
beauty accumulation uses — position within `max(0.03, 0.02·d)`, normal dot ≥ 0.9,
roughness delta ≤ 0.12 — so a disocclusion cannot inherit a stranger's sample.
And a shadow ray from $x_1$ to $x_s$ zeroes $\hat p'$ when the reused vertex has
become occluded, stopping light bleeding through a wall that closed between
frames:

{{cite shaders/rt/pt_raygen_realtime.rgen "if (payload.hitDist >= 0.0) pHatPrev = 0.0;"}}

Inherited confidence is then clamped to twenty times the current frame's:

{{cite shaders/rt/pt_raygen_realtime.rgen "float mClamped = min(prevR.M, 20.0 * giCurr.M);"}}

That clamp also bounds the bias, because both combiners are M-weighted, not
MIS-weighted: a candidate enters carrying $\hat p\cdot W\cdot M$, no balance
term. The shader calls the temporal one biased — unbiased only in the limit, and
only because that reuse is same-domain —

{{cite shaders/rt/pt_raygen_realtime.rgen "Combiner is the biased M-weighted RIS combiner"}}

— and repeats the label for the spatial fold, where the sample crosses to a
different primary hit and that argument lapses:

{{cite shaders/rt/pt_raygen_realtime.rgen "Fold neighbor into the spatial reservoir (biased M-combiner"}}

GRIS pairwise or generalized-balance weights would remove it; none are computed
here.

:::key
`W` is neither a colour nor a probability. It is the scalar that re-weights a
sample taken for a *different* shading point — valid only because $\hat p$ was
recomputed against the current surface. Cache the old $\hat p$ and the reservoir
becomes a slow, wrong blur that looks plausible.
:::

## Spatial reuse rides one frame behind

Phase 2 folds in K = 4 neighbours from a screen-space disk whose radius shrinks
15 % per tap — 20, 17, 14, 11 px:

{{cite shaders/rt/pt_raygen_realtime.rgen "float rr = RADIUS * (1.0 - 0.15 * float(k)) * sqrt(du.x);"}}

Its centre is not this pixel but the primary hit reprojected into the *previous*
frame's screen space — the frame those reservoirs belong to — or `pixel` if that
reprojection leaves the frame:

{{cite shaders/rt/pt_raygen_realtime.rgen "baseCenter = clamp(ivec2(sUV * vec2(pc.params.xy)),"}}

A neighbour's sample was integrated over *its* solid angle at *its* primary hit,
so reuse needs the change-of-measure Jacobian (Ouyang et al. Eq. 11):

$$J=\left|\frac{\cos\phi_r/\lVert x_s-x_{1r}\rVert^{2}}
{\cos\phi_q/\lVert x_s-x_{1q}\rVert^{2}}\right|$$

$x_{1r}$ is this pixel's primary hit, $x_{1q}$ the neighbour's; each $\phi$ is the
angle at $x_s$ between $n_s$ and the direction back to the corresponding primary
hit. Without $J$, samples reused across a depth or grazing-angle discontinuity
are weighted as if they subtended the same solid angle at both receivers —
silhouettes band light or dark. The ratio is then clamped to $[10^{-3},10^{3}]$
as numerical insurance:

{{cite shaders/rt/pt_raygen_realtime.rgen "return clamp(J, 1e-3, 1e3);"}}

:::why
One raygen dispatch covers the whole ReSTIR pipeline, so a thread's neighbours
have no current-frame reservoirs yet. The textbook fix is a second pass with its
own pipeline, SBT and descriptor plumbing. The engine instead reads the previous
frame's temporal reservoirs at 29–31 with their surface and shading history:
spatial reuse for zero new GPU passes, one frame stale. Under a moving camera
that cost is never paid — reuse stops entirely.
:::

Both paths carry the same `!viewChanged` gate:

{{cite shaders/rt/pt_raygen_realtime.rgen "bool giReuse = (!restirGiOff) && (historyFrameCount > 0u) && (!viewChanged)"}}
{{cite shaders/rt/pt_raygen_realtime.rgen "bool spatialOn = (!restirGiOff) && (!restirGiNoSpatial)"}}

`viewChanged` is `pc.control.z`, from `m_viewChangedThisFrame`:
`notifyViewChanged()` raises it, `render` clears it each frame, `interactive`
sets it whenever the camera moved. A continuously moving camera therefore runs
neither pass; what surfaces is a noisy single-frame RIS estimate, not
stale-neighbour lag.

Only the *temporal* reservoir goes back to the ping-pong planes, captured before
any neighbour is folded in; the spatial result is shading-only:

{{cite shaders/rt/pt_raygen_realtime.rgen "imageStore(currGIReservoir0, pixel, vec4(merged.xs, merged.M));"}}
{{cite shaders/rt/pt_raygen_realtime.rgen "GIReservoir spatialR = merged;"}}

## The wiring HEAD no longer has

Everything above is live GLSL and the layout still reserves 29–34, but the host
code that joined them is gone: the descriptor update in `PathTracer::render`
builds a 29-entry write array and stops at binding 28, and no `dstBinding` above
28 exists anywhere in the tree.

{{cite ohao/render/rt/path_tracer_render.cpp "VkWriteDescriptorSet writes[29] = {};"}}

`m_giReservoirViews` appears only at image creation and destruction, no barrier
moves those images to `VK_IMAGE_LAYOUT_GENERAL`, and `m_giReservoirWriteIndex` is
assigned `0` in three places and incremented in none — the ping-pong cannot
alternate. The shader's four `PT_FLAG_RESTIRGI_*` bits, 5–8 of `pc.control.x`,
are set by nothing.

No commit deleted this. The merge `c894ebf` took `path_tracer_render.cpp` from a
branch forked at `1910fd9`, two hours before ReSTIR landed in `cd87dea`: parent
`a726db3` still holds `writes[40]` and a live `m_giReservoirWriteIndex = 1u -
m_giReservoirWriteIndex`; the merge result holds neither, and `cornell_box` drops
from nine `restir` matches to one, `restir_probe` with it.

Bindings 29–34 carry no `PARTIALLY_BOUND` flag — that is on binding 12 alone — so
this is not a legal "optional descriptor" configuration. A statically-used
descriptor that is never written is undefined behaviour; what a driver does with
it is not characterised here.

## Two sharp edges in the layout

The storage-image pool is sized to exactly the 25 storage-image bindings, six of
them reservoir planes:

{{cite ohao/render/rt/path_tracer_descriptors.cpp "{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 25},"}}

No slack: a 26th storage image added without editing that literal fails
`vkAllocateDescriptorSets` with `VK_ERROR_OUT_OF_POOL_MEMORY` at startup, nowhere
near the cause.

The second edge predates ReSTIR but the reservoir block widened it. Vulkan
requires `VARIABLE_DESCRIPTOR_COUNT` on the set's highest-numbered binding; here
it sits on the bindless texture array at 12, with 23 higher-numbered bindings
above:

{{cite ohao/render/rt/path_tracer_descriptors.cpp "bindingFlags[12] = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT"}}

The comment directly above that line still claims the flag is on "the LAST
binding only".

## Contracts

- $\hat p$ must be re-evaluated at the current primary hit before the held sample is shaded. Reusing the stored $\hat p$, or a stale `W`, distorts the GI while still looking smooth.
- The spatial pass must not write back into the ping-pong planes; persisting it compounds sample correlation until the GI bakes.
- Bindings 29–34 must be written every frame with prev/curr swapped and the images in `VK_IMAGE_LAYOUT_GENERAL` before the raygen runs. Neither happens in HEAD.
- The `{STORAGE_IMAGE, 25}` entry is exact: bump it with `bindings[36]`, `flagsInfo.bindingCount` and `layoutInfo.bindingCount`. The `29..34` loop assumes array index equals binding number.
