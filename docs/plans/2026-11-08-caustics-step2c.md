# Caustics step 2c (normal-mapped glints) Implementation Plan

**Spec:** `docs/specs/2026-11-04-caustics-step2-design.md`, "Step 2c: normal-mapped glints" (T1 and T2 measured;
rulings M10, M11, M11b, M13 accepted).

**Goal.** Step 2a's term (sun -> a smooth reflection -> X's rough part) for normal-mapped casters, every root of every
cell found, so the showcase's helmet glints stop being the camera side's dots (ruling M7 lifted).

## Global constraints

- Exact: every root of a cell is found or the cell is counted undecided (the closed forms must show none). No cap that
  drops roots, no filtering of the map.
- **Bounded work per lane per dispatch** (2b's lesson, 2026-11-08): a GPU dispatch is not preempted, so no thread may
  run an unbounded loop. Work that can be large (cells a request, boxes a cell) is split into items of bounded cost
  and scheduled across threads and dispatches, never looped over in one lane.
- One solver, two builds: Slang compiled to SPIR-V and to the C++ twin (`tools/caustics/manifold_kernel.slang`), tested
  on the CPU first (`manifold_tests`, t-numbers `m1x_*` as the spec's).
- fp32 on the GPU; the twin's tests compare against double precision.
- Each OpenPBR entry keeps one call site per kernel (AGENTS.md, "Every call site is compiled whole").

## Tasks

### Task 1: the cell's polynomial in Bernstein form (CPU twin) — built 2026-11-08

`shaders/rt/manifold_poly.slang`: on one cell (the square between four texel centres, (s, t) in [0, 1]^2) of a
triangle, MikkTSpace's mapped normal `m = tn.x vT + tn.y vB + tn.z vN` from the interpolated vectors as they are (vB the
interpolation of the per-vertex `sign N x T`), the drawn slope's microfacet normal `h = m - sigma.x vT - sigma.y vB`,
`d = p - X`, and the two constraints `F1 = (d.a)(h.h) - 2 (d.h)(h.a)`, `F2` with b (a, b perpendicular to L): tensor
Bernstein grids of degree (5, 5) built from the cell's corner values by Bernstein products (convex combinations only),
in fp32. Twin op 16 returns the grids and F at a point.
- `m11_poly_matches_map`: at random points of random cells, F from the grids (de Casteljau) equals the double
  formula within fp32's rounding of its terms, and `F = 0` exactly where the double reflection holds (r parallel to L).

### Task 2: every root of a cell: Bernstein subdivision and Krawczyk (CPU twin) — built 2026-11-08

A box stack over [0, 1]^2 (the stack holds boxes only: each box's grids are extracted from the cell's by two de
Casteljau splits a direction). A box is excluded when 0 is outside either grid's hull (with a margin of the grids' own
rounding) or Krawczyk's K misses it; proven when K lies inside it (Newton from the centre); else split along its wider
side. Roots kept inside the cell, the triangle and their box's half-open extent (no root twice), and physical (r . L
> 0, h on the normal's side). A box budget; undecided boxes counted.
- `m12_cells_complete` (the spec's m10): T2's random cells at 27 and 45 degrees: every root a dense double search finds
  is found, every root reported a root of the grids within mu of the double polynomial's, undecided 0; boxes a cell
  (median, mean, max) printed. Results: the spec's "Step 2c", T3.

### Task 3: MikkTSpace's normal in both renderers (ruling M13)

The G-buffer, the tracer's hit (`fillHitAt`), RT GI's and RT direct light's hits: `normalize(tn.x vT + tn.y vB + tn.z
vN)` from the interpolated vectors, vB interpolated from the vertices. Measured first: every normal-mapped golden's
move (`render_golden.py --json`), looked at, then updated. The tracer's lift (a normal below the geometric plane) is
kept or dropped by what the cell's polynomial can carry: decided here, with numbers.

*As built (2026-11-17).* `mikkTSpaceNormal` (material/material_layers.slang): per vertex the unit world normal, the unit
world tangent (through the world matrix, not the normal matrix: a tangent lies in the surface) and `w n x t`, interpolated
as they are, the result normalised; seen from behind the whole normal turns. The G-buffer's vertex stage writes the
bitangent as an interpolant; the tracer's hit (`mikkFrameAt`) builds the same three from the vertices. RT GI and RT
direct light read no map (the geometric normal; the G-buffer). The solver's `ManifoldMappedTri` carries the per-vertex
bitangents (it took vertex 0's sign); T1's rejected cell map (`CellSlopeMap`) is deleted.
- **Found: the tangent's handedness was never cooked.** The cooker set w from the node's mirror alone, dropping the
  importer's bitangent, so every UV-mirrored half was lit with its bitangent upside down. w is now the side of `n x t`
  the importer's bitangent lies on, after the node's transform (`kCookerVersion` 4).
- **The helmet's golden** (the only normal-mapped one) moved: 18.9% of the pixels, max 61, RMSE 1.57, the left half
  of the helmet (its UVs mirror the right). A/B: the formula with the old handedness moves 4.1% of the pixels, max 29,
  RMSE 0.16, symmetric; the rest is the handedness. Looked at, updated. The other four goldens exact.
- **The lift** stays in the tracer for now: the pass counts the roots whose normal it would lift (task 6), and the
  choice is made on that count.

### Task 4: the map read in fp32 (ruling M10)

Owned normal-mapped materials read their map as four `Load`s blended in fp32 in the tracer's hit and the pass (the
same expression), not the hardware's 1/256-quantised filter. `m13_fp32_normal`: the tracer's normal equals the twin's
to 1 ulp. Goldens with normal maps measured and updated after looking.

*As built (2026-11-17).* `mapNormalAt` (rt/pt_hit.slang): the four texels round uv at mip 0, each decoded by
`normalFromMap`, blended bilinearly in fp32, repeating as the sampler does; the pass calls the same function, so the two
agree by construction (no twin since 2026-11-09). Every normal map the tracer reads takes it, not owned materials only:
one path, and no golden path-traces a normal map (all five exact). Checked against the hardware filter on the helmet
(640x360, 64 spp, one seed): mean within 6e-6, mean absolute difference 6.5e-5, 0.5% of the pixels more than 1% apart;
a half-texel shift would move every bump.

### The pass as rounds (decided 2026-11-17, before tasks 5 and 6)

**Where it starts.** `manifoldConnect` (rt/path_tracer.slang) is one thread a pixel: for each request and light it
picks one owned draw by solid angle and walks that draw's whole hierarchy in the lane (groups, clusters, sub-clusters,
triangles; 2a's disc test at each), then for each candidate triangle Newton, a visibility ray, the retrace of y, the
connection, summed into the pixel. Its work is bounded only by the chosen draw's size: tolerable for 2a's spheres,
not once 2c owns Sponza's draws and the helmet (tens of thousands of triangles each, their cones widened by the map),
and not with cells (47 a request on the helmet, up to 425, each 27 to 620 Krawczyk boxes). Step 2b's in-lane loop of
up to 4,096 walks is the one that took the GPU's firmware down twice.

**Decided:** the pass becomes rounds of items of bounded cost, in queues, across dispatches; 2a's walk moves onto them
first, so the machinery is written once and 2c's cells are one more kind of item.
- **Setup** (raygen, a thread a request): retraces X, picks the draw and the slope (the same hashes as today), writes
  a request record and the draw's group items.
- **Walk** (compute, a thread an item): an item is (request record, light, node); it tests the node's children (at
  most 8 clusters, 8 sub-clusters or 16 triangles) and appends those that pass to the next round's queue. The rounds
  follow the levels: groups, clusters, sub-clusters, triangles; for a mapped triangle, its uv footprint's pyramid
  nodes down to cells.
- **Solve** (compute): a plain triangle, 2a's Newton (16 steps); a cell, `cellPolySolve` with a box budget an item,
  set by measurement; a cell at the budget is a counted miss. Roots go to the connect queue.
- **Connect** (raygen, a thread a root): the shadow ray, the retrace of y, X's surface again (its retrace from the
  request), the contribution added to the pixel as fixed-point integers (`InterlockedAdd`: the sum is the same in any
  order, so two captures stay identical; a clamp is counted), resolved into the accumulation by one pass.
- **Bounded:** each dispatch's work per thread is a small fixed maximum; queue capacities are set by measurement and
  an overflow is counted, never silent; dispatches are indirect from the queues' counts.
- **Proof for 2a's move:** the same scene and seed give the same candidates, roots and connections (counted exactly:
  the same tests on the same numbers) and the same image within the fixed point's rounding.

*Stage 1 as built (2026-11-17,* `ohao/render/rt/manifold_rounds.*`*, `f9ddc268`):* Setup, three walk rounds (groups,
clusters, sub-clusters), Solve, Connect, each a raygen dispatched indirectly from the queues' counts
(`vkCmdTraceRaysIndirectKHR`), then Resolve (compute). Items are (record, kind << 30 | index), kind 1 left for cells; a
root carries its map so Connect does not run it again. The frame runs in bands of whole rows (at most 131,072 pixels
a band), so the queues' memory (about 150 MB) does not grow with the resolution; capacities a pixel from the fullest
band measured (records 2, groups 16, clusters 16, sub-clusters 48, candidates 24, roots 2), 1.7x to 2.5x the 1080p
showcase's peaks; an overflow is counted and logged (a denser scene than measured would lose that light: open). The
sum is fixed point by integer atomics (rounded by a hash of the item): two captures identical. Step 2b's glass stays
in Setup, into the same sum.
- **Proof:** at 1920x1080 against step 2a's own `manifoldConnect` compiled beside it, requests, roots, connections,
  cluster and sub-cluster tests identical; candidates 12,039,527 -> 12,039,521, triangle tests 322,526,700 ->
  322,526,684 (tests at their thresholds: about 1,400 a capture sit within 1e-6); the image max abs 1.8e-3, mean abs
  2.2e-6. The caustics scene: every counter identical. Glass 640/13/13/0 as before. Synchronization validation silent.
- **Cost of `pt.manifold`** (the showcase): 640x360 about 92 -> 8.0 ms; 1920x1080 644 -> 72-76 ms (setup 17, walk 39,
  solve 8, connect 6). Its cold compile 4.3 -> 1.8 s.
- **Found: 2a loses roots at fp32's noise floor.** Newton accepts a root at |sigma - target| <= tol, and tol is 1e-6,
  sigma's own rounding. Against the same Newton in double on every candidate (160x90): the double finds 1,989 roots,
  the raygen build 1,984, a compute build of the same SPIR-V 1,948; neither accepts one the double rejects. The
  stages differ because the driver lowers `dot(v, v)` in a different order (an fma chain from x or from y; a few ulp
  through `manifoldMap`'s lengths), which is why the rounds are raygens. To be fixed by proof, not a wider tolerance:
  a root accepted by a proof, so the count does not depend on rounding.
- **No light lost when a queue is full** (stage 1b): each band runs in 12 passes; an item whose outputs do not fit is
  carried, with a resume index, into its queue's next pass (two halves a queue: this pass's input, the next's
  carries). Setup queues one record item; a first round emits the record's groups, 256 a pass. Lost is only what is
  still carried after pass 12 (counted). Planted at 1/16 of the 1080p showcase's queues: 10 passes, 3.44 M items
  carried, none lost, the image and counters 1-7 identical to the unplanted run. Cost: `pt.manifold` 1080p 72-76 ->
  83 ms (the empty passes about 6 ms), 640x360 8.0 -> 9.3 ms.
- **2a's triangle certifier is not the proof** (measured, reverted): Krawczyk on the triangle's slope map
  (`manifoldCertify`) in Solve, against a double-precision search of 2,002 roots at 160x90: 1,945 proven, 57 missed,
  none false (Newton today: 1,984). Around every missed root K(B) neither fits nor excludes down to the minimum box:
  grazing reflections (|H . n| at the 10th percentile 0.03), where the map's interval Jacobian stays wide at any width.
  T1's finding again (the map's natural enclosures overestimate). **Decided:** a plain triangle is a normal-mapped
  cell whose texels are (0, 0, 1); 2a's triangles go to the polynomial solver with 2c's cells (stage 3), one exact
  solver; until then Newton, its 0.25% loss recorded.



A pyramid of normal cones per normal map, built at upload; a triangle's cone composed with the pyramid's over its uv
box, 2a's disc test at each node down to cells. Proven conservative on random triangles and maps
(`t8_disc_conservative`'s method). Measured on the helmet: cells a request (T1 counted 47 at least, 0 to 425).

*Stage 2 as built (2026-11-17; the walk opt-in, for measurement):* the pyramid holds balls of tangent normals, not
cones (a ball survives the slope's subtraction `tn - s`; a cone of directions does not), built on the GPU at upload
from the tracer's own `Load` + `normalFromMap` (`manifoldPyramidBuild`), so the BC5 decode is the GPU's by
construction: no format or cooker bump. A leaf covers 4x4 cells (one cell a leaf would be about 4x the memory): 23.2 MB
for the showcase's 14 maps (the helmet's 2048^2 5.33 MB, a 1024^2 Sponza map 1.33 MB), built in 0.97 ms once. A
mapped triangle's frames and uvs (9 float4) sit after the plain slots in a side table (the helmet: 112,512 slots,
15.45 MB). The bound: nodes and triangles `|h - M'(c - s)| <= g rho + delta (|c - s| + rho)`, then
`manifoldConeTest` at angle 0; a cell, the hull of its four texels projected along the half vector, with delta and the
half vector's spread as slack. 2a's disc term is not conservative in the vertex tangent frame's slope and is not used
on mapped triangles. Mapped materials have their own owned bit, off unless `OHAO_MANIFOLD_MAPS=lo,hi` (map widths);
with it every frame, the showcase took 870 ms at 320x180 and 99.75% of requests picked a mapped draw, so the image is
unchanged by default. Measuring: `OHAO_MANIFOLD_SPARSE=k`, `OHAO_MANIFOLD_CELL_STATS=1`, `OHAO_MANIFOLD_DUMP=x,y|any`.
- **Cells a request** (sparse grid, nothing lost): the helmet 11,594 mean / 11,264 median / 17,135 max at 320x180,
  11,945 / 10,880 / 18,884 at 640x360; Sponza 15,789 / 1,536 / 118,366 and 11,362 / 2,240 / 64,273. Pyramid tests a
  request: the helmet 286 K, Sponza 665-912 K (max 6.2 M). About 6,700 helmet triangles a request pass the triangle
  test. T1's 47 counted cells whose four corners' slopes bracket the drawn slope, which is not a conservative test.
- **Proofs:** the bound, a float32 copy of the GPU's code against double samples: 142,935 samples in 2,667 regions,
  none outside (worst margin 0.019 rad). Completeness, a CPU brute force over every cell of every helmet triangle
  (6.33 M pairs, 4 dumped requests): on a quarter-texel grid of points in the triangle, the line
  `s + lambda M(y)^-1 (normalize(p - y) + L)` meeting the hull of the cell's four GPU-decoded normals flags 680-986
  cells a request; the walk emitted 10,670-11,801, none flagged and not emitted (sampled points: strong evidence, not
  a proof). Walk off: the caustics scene and the showcase (320x180, 4 spp, twice) counters and PFMs byte-identical to
  stage 1b. Synchronization validation silent with the walk off and on. `pt.manifold` with the walk off 1.51 -> 1.62 ms
  (caustics), 2.64 -> 2.84 ms (showcase) at 320x180.
- **Refuted: the exact cell route does not fit a frame.** At 640x360 the helmet alone is 26-100 K requests; the walk
  at about 1.1e9 tests/s is about 26 s a frame, and the solve, 11.6 K cells x about 30 boxes x about 2,000 flops, about
  0.7 Gflop a request, 3-4 orders over. The walk is about 12x looser than the brute force's test (delta and the half
  vector's slack at a cell's size); the floor (cells that hold a root) is not yet measured. Found on the way: a
  point-in-hull test that called four equal normals inside every degenerate triangle (the GPU's and the brute force's;
  16.5 K -> 11.6 K cells), and a write-after-write on the pyramid's directory (synchronization validation).
- **The floor (2026-11-17, CPU, the 4 dumped requests):** a dense double Newton search (6x6 seeds; 12x12 found the same
  2,848 roots) over all 45,271 emitted cells: 171-391 cells a request hold a real path root (mean 288; `r.L > 0`,
  `h.H > 0`), 458-652 a root of either sign (half are flipped, `h.H < 0`, which every test so far accepts); 44,368 cells
  near the hull but not emitted hold none. The brute force's hull test is not a necessary condition (310 of 2,401 root
  cells unflagged, near cell edges). Where the walk's cells come from: six uv-degenerate triangles (two equal uvs) emit
  79% (about 8,900 a request: kept whole over their uv box, about 6,200 cells for one, though their uvs cross about 510)
  and hold no root; on the rest (2,381 a request) delta dominates (55x dh at the median); the hull of the products
  `M(y_k) v_j` over the region's vertices and the four texels, conservative, gives 1,195 and keeps every root cell.
  Per level the walk passes 20-40x the nodes that hold a root cell.
- **Decided (2026-11-17): the exact route stays, for the path tracer.** The pass runs only in the path tracer, an
  offline renderer, so the bar is not a real-time frame but convergence against the light pass at equal time (task 7).
  Estimated, at about 30 boxes a cell, 2,000 flops a box and an assumed 5 Tflop/s: the helmet at 640x360, 26-100 K
  requests, is 3.5-14 s a frame with today's walk, 0.5-1.8 s with the degenerates and delta fixed, 90-350 ms at the path
  floor. Next (stage 2b): uv-degenerate triangles walk only the cells their uvs cross, the product hull replaces delta,
  the sign tests (`h.H > 0`, `r.L > 0`) prune at the cell and where a bound allows at the nodes; measured on the GPU
  against the floor's root cells (every one emitted). Then stage 3, then task 7's equal-time comparison decides whether
  mapped draws are owned by default.

*Stage 2b as built (2026-11-17; the walk still opt-in):* `manifoldMapClip` clips a triangle by a uv rectangle's four
sides in its own (u, v) (uv is affine in them: each side a half-plane, no inverse), so a uv-degenerate triangle keeps
only the strip whose uvs cross the cell, at nodes and cells alike. `manifoldMapHullTest` replaces delta at triangles,
pyramid nodes and cells: M is affine over the region and tn a convex blend of the texels' normals within rho, so h lies
in the hull of the products `M_k (c_j - s)` grown by `rho max_k |M_k|`; seen along Hc (within dh), a root needs the
projected hull within `tan(dh)(D + R) + R` of the origin, tested by a 2D GJK (at most 16 steps, 32 points) that prunes
only on a separating line. The gather's BVH nodes keep delta (their region's vertices are not at hand). Oriented (word
39; `OHAO_MANIFOLD_ORIENTED=0` off): a region is pruned when `max P . Hc + R <= 0`, nodes drop the flipped branch;
`r . L > 0` follows from h parallel to H (`r = 2 (1 + w . L)|d| lambda^2 L`; 0 of the floor's 2,848 roots violate it).
- **Cells a request** (stage 2 -> 2b): the helmet 11,594 -> 532 mean (512 median, 818 max) at 320x180, 11,945 -> 516
  at 640x360; Sponza 15,789 -> 6,623 and 11,362 -> 4,070 (medians 512, 435). Pyramid tests the helmet 286 K -> 119 K,
  Sponza 912 K -> 659 K. The pyramid stage's time the helmet 5.5 -> 3.5 ms and 9.3 -> 5.7 ms; Sponza 4.7 -> 7.4 ms and
  8.9 -> 9.9 ms (a hull test costs more than the ball test it replaced).
- **Left:** cells 2.0x the path floor (582 oriented against 288); nodes 20-46x at levels 0-5, from the map's own spread
  inside the finest node (zeroing R takes leaf passes 2,824 -> 12; a leaf's ball median radius 0.585), which no convex
  bound a node removes. Not built: the uv box's texels in place of the footprint's union ball (radius 0.93 -> 0.50, an
  estimated 3.6x fewer triangle passes).
- **Proofs:** completeness against the floor, the same 4 requests re-dumped: oriented, every path cell emitted
  (171/391/355/236 of 368/756/693/511); unoriented, every root cell of either sign (458-652 of 886-1,320). The bound's
  fp32 copy against planted roots, a third of the triangles uv-degenerate: 17,292 regions, 273,691 roots, 410,978
  tests, none pruned (worst dh margin +1.1e-4 rad). Walk off: the caustics scene and the showcase byte-identical.
  Synchronization validation silent, walk on and off. Cold compile 1.9 s.

*Stage 3 as built (2026-11-18): one exact solver.* Solve runs Bernstein subdivision and Krawczyk (T3) on cells and on
plain triangles; 2a's Newton (`manifoldSolve`, `manifoldMap`, its tolerance) is deleted.
- **A plain triangle is a cell** on its own (u, v) square with texels (0, 0, 1) and 2a's frame at each vertex
  (`polyTriangleSource`), interpolated: h is then a blend of three equal-length 2a half vectors, so 2a's cone and disc
  tests stay conservative (3.04 M samples, worst margin -2.6e-8 rad against the test's own 1e-4 slack). The roots are
  this frame's, not 2a's: the per-candidate counts move.
- **Bases:** an undecided box no wider than 1/8 of its base is solved on dyadic sub-squares with their own grids (3
  levels): a whole triangle has folds where the extraction margin exceeds F (on the CPU, 3 of 2,000 random triangles
  reached a 100 K-box cap before; none undecided after).
- **The weight** `|dA/dsigma| = areaST |det dF/dsigma| / |det dF/d(s, t)|` (implicit function theorem, from the
  derivative grids; `dh/dsigma = -(vT, vB)`): against double finite differences median relative error about 1e-6, p99
  5e-5, worst 0.14 at folds.
- **The slope draw is untruncated GGX:** a draw truncated to the core's disc in a frame other than 2a's misses the
  core's stretched edge (biased); at most 1% more draws.
- **Bounded:** at most 96 Krawczyk tests an item a dispatch (tests an item: helmet cells median 6, p99 76, max 389;
  plain median 1, max 208-340), 12 passes cover every maximum; an unfinished item carries a stack (at most 32 entries,
  14 measured) and its roots, 768 B, through the cells' queue; still open after the last pass is counted. Planted at
  B = 4: 2,914 carried, PFMs byte-identical, 15 open and counted.
- **Roots, plain, against a dense double search in the same frame:** the caustics scene 2,027/2,027 (160x90) and
  34,093/34,093 (640x360); the showcase 8,686/8,686 and 139,330/139,331, none false, none open. Stage 1's "0.25% loss"
  was against Newton in double, itself short: the dense search finds 2,027 where Newton found 1,984.
- **Cells against the floor:** 1,164 of 1,164 path roots, none false, none open (dumps from before the draw change).
- **m9_bumped_mirror as a CPU check** (no scene: primitive meshes carry no uvs or tangents, `Material` no texture
  slot): a sine height field's map on a flat mirror, every cell by the fp32 CPU copy of the solver, against quadrature
  in double: 0.9959 +- 0.49% (alpha 0.1), 1.0024 +- 0.31% (alpha 0.01).
- **Image, walk off, against stage 2b** (320x180, 4 spp): the caustics scene 183 pixels (0.32%) differ, at most 1.7e-3;
  the showcase 3 pixels, one fixed-point quantum; means within 1e-6. Walk on (sparse 16): 117,728 cell roots, 1,772
  connections (30 without maps), 7 of 240 pixels change, at most 1.6e-4: no bright dots at these receivers.
- **Cost** (`pt.manifold`): the caustics scene 1.6 -> 11.5 ms (320x180), about 45 ms (640x360); the showcase 2.8 -> 10.8
  ms, about 42 ms, 1080p 83 -> 337 ms (solve 253, against Newton's 8). Compile-time grid degrees took a solve from 70 to
  2.9 ms (160x90). Cold compile 1.9 -> 3.4 s.
- **Open:** one root of the showcase's 139,331 missed at 640x360 (its nearest found 1.1e-3 away; a fold, unconfirmed);
  Sponza with the walk on carries for 73 ms and leaves 4 items open (11 at 640x360), cause unconfirmed; the solve costs
  about 30x Newton.

*Stage 3b (2026-11-18): the three open items.*
- **The missed root was cancellation:** `d = y - p` with y rounded at |y| (2.3) and the receiver 0.02 away moved an fp32
  root by 3.7e-5 of the triangle's side, to u = -6.7e-6, outside it; the certified root was then dropped by
  `polyPhysical` and the half-open ownership test. `CellPoly` holds `d[k] = (p0 - p) + e1 u + e2 v` (`p0 - p` exact when
  the receiver is near, Sterbenz), so the rounding is relative to |y - p|. On a CPU set biased to it (receivers close,
  roots 1e-7 to 1e-3 inside an edge) losses 244 -> 65, every one left within 1.6 floor units of the edge (eps |J^-1| |F|;
  up to 70 before). On the GPU against the dense double search: the caustics scene 2,027 and 34,093, the showcase 8,686
  and 139,331, every root found, none false, none open. Cells: 1,373 of 1,373 path roots of a fresh floor.
- **Sponza's open cells** needed 813-1,174 Krawczyk tests (sharp brick edges near folds), and arrived only in passes
  0-2 because the pyramid queue overflows (about 1.41 M nodes against 460 K). A carried item's stack splits into its
  groups (a box, or an inflation box with its halves), each its own item and state while states are free: disjoint
  boxes, so no duplicate roots; a part whose stack outgrows a state is settled open and counted. Open 4 -> 0 (320x180)
  and 11 -> 0 (640x360), at most 9 passes; carry 81 -> 62-72 ms and 50 -> 36-42 ms.
- **The solve's cost** (the showcase 320x180, 7.7 ms measured by part): zeroing the 64-entry stack 3.5 ms (removed);
  Newton's polish 1.5 ms, 8-17% of roots running all 40 iterations at fp32's floor (it stops when a step stops
  shrinking, at most 13); base grids 2.4 ms, extraction 1.2 ms. Plain triangles get grids of their true degree 3 in
  their own raygen (`manifoldSolvePlain`; cells `manifoldSolveCells`: 255 registers, 271 KB). Three other restructures
  measured slower, reverted. Solve the caustics scene 9.6 -> 2.3 ms (320x180), 37.9 -> 9.0 (640x360); the showcase
  6.8 -> 1.8, 29.8 -> 5.8, 1080p 253 -> 60 (`pt.manifold` 337 -> 143 ms). The helmet with the walk on 12.9 -> 8.2 ms
  (320x180). Cold compile 3.4 s.
- **Image, walk off, against stage 3:** the showcase identical at 320x180; the caustics scene 46 pixels, at most 4.9e-4,
  and at 640x360 55 and 5 pixels of one fixed-point quantum (area weights moved about 1e-6 across the stochastic
  rounding; the root sets identical at 160x90). Two captures identical.
- **Open:** a denser walk-on run (maps 1024 and 2048, sparse 32, under sync) lost 960,946 pyramid nodes still carried
  after pass 12 and left 231 cells open (counted); parts draw states from the same pool. The pyramid queue's capacity
  and the state pool are the walk's next limit, before mapped draws can be owned by default.

### Task 6: the pass on the GPU, bounded

Requests -> candidate cells -> cell solves as work items of bounded cost (a box budget an item), spread across threads
and dispatches with the leftover carried, never a loop over a request's cells in one lane. Owned bit (M7) drops "no
normal map". Cost per stage measured on the showcase (`render.capture {timings}`) before anything is tuned.

### Task 7: closed forms, showcase, docs

`m9_bumped_mirror` (the spec's: a sine height field's map on a mirror, the wall's light against a numpy enumeration
of every cell in double, 1%), the showcase's helmet dots at equal time, the change record.

## Order

1 -> 2 on the CPU (no GPU risk), then 3 -> 4 (goldens, one GPU job at a time), 5, 6, 7.
