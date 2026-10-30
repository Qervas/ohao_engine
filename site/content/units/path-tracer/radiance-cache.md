---
module: path-tracer
id: radiance-cache
title: Radiance cache
standard: v2
---

## What it is for

The realtime profile traces two bounces a frame. Light that needs more bounces than that would
be missing from the image, so a realtime path that reaches its second or later vertex can end
there and take, instead of tracing on, the radiance that surface reflects from a world-space
cache. The cache is filled by a sparse trace of its own each frame and holds every bounce,
because each sample that fills it may itself end in the cache.

{{cite ohao/render/rt/rt_settings.hpp ".maxBounces = 2,"}}

The design is NVIDIA's SHaRC (a hash grid of radiance, filled by sparse long paths, read by
short ones); the code is OHAO's own Slang, in `shaders/rt/radiance_cache.slang` (the cell, the
hash, the integer sums), `shaders/rt/radiance_cache_resolve.slang` (the per-frame blend) and the
path tracer's `cacheUpdate` raygen. `render::RadianceCache` owns the buffers and the resolve
pipeline. The offline profile never reads the cache: it stays the unbiased reference.

{{cite ohao/render/rt/rt_settings.hpp ".radianceCache = true,"}}

## The cell

A cell is a voxel at one of 15 levels, plus the side of the voxel a surface faces. The level is
camera-relative: 0 within a metre of the camera, then one more for every doubling of the
distance, and a voxel at level $l$ is $0.05 \cdot 2^l$ metres on a side, so cells subtend
roughly the same angle at any distance.

$$
l = \min\!\big(\lfloor \log_2 \max(d, 1) \rfloor,\ 14\big), \qquad s_l = 0.05 \cdot 2^{l}\ \text{m}
$$

{{cite shaders/rt/radiance_cache.slang "uint cacheLevel(float d) { return min(uint(max(floor(log2(max(d, 1.0))), 0.0)), kCacheMaxLevel); }"}}

{{cite shaders/rt/radiance_cache.slang "static const float kCacheVoxelNear = 0.05;"}}

The side is the normal's dominant axis and its sign, six values. A thin wall's two faces fall
in the same voxel; the side keeps the light of one face out of the other.

{{cite shaders/rt/radiance_cache.slang "uint cacheSide(float3 n)"}}

## Keys and slots without 64-bit atomics

The voxel's three integer coordinates, the level and the side are hashed into one 32-bit key,
forced odd so it is never 0 (0 marks a free slot). A second hash of the same value picks the
first slot, and a lookup probes 8 consecutive slots from there in a power-of-two table.

{{cite shaders/rt/radiance_cache.slang "h = cacheHash(h ^ (level * 8u + cacheSide(n)));"}}

{{cite shaders/rt/radiance_cache.slang "const uint key = h | 1u;"}}

{{cite shaders/rt/radiance_cache.slang "static const uint kCacheProbes = 8u;"}}

Inserting claims the first free probe with a 32-bit compare-exchange; a slot that already
holds the same key (another thread claimed it first) is taken as found. When all 8 probes hold
other keys the insert fails and the sample is dropped. Two cells whose 32-bit keys collide
share a slot: the key is the whole identity, there is no second word to compare.

{{cite shaders/rt/radiance_cache.slang "InterlockedCompareExchange(keys[slot], 0u, cell.x, previous);"}}

{{cite shaders/rt/radiance_cache.slang "if (previous == 0u || previous == cell.x) return slot;"}}

## Integer sums

A frame's samples are summed in a `uint4` per slot: the radiance in units of 1/1024 and the
sample count, each added with `InterlockedAdd`. Integer addition gives the same sum in any
order, so the cache after a frame does not depend on which thread got there first, and a
capture repeats bit for bit. A float atomic sum would not.

{{cite shaders/rt/radiance_cache.slang "const uint3 q = uint3(clamp(radiance, 0.0, kCacheMaxRadiance) * kCacheScale + 0.5);"}}

Each sample's radiance is clamped to 2000 first, so one sample adds at most 2,048,000 to a
channel and a slot's 32-bit channel overflows only past about 2,097 such samples in one frame.

{{cite shaders/rt/radiance_cache.slang "static const float kCacheMaxRadiance = 2000.0;  // a sample's radiance is clamped to this (no uint overflow)"}}

:::why
Integers buy determinism, which the gates need: a lockstep capture must equal itself and a
window's capture of the same world. The price is the clamp, a bias on very bright samples, and
a quantum of 1/1024 per sample.
:::

## Resolve

After the update trace, one compute thread a slot blends the frame into the slot's resolved
radiance. The frame's mean is weighted by its sample count against the count the slot already
holds, and that count is capped at 256, so a cell's history is at most 256 samples and old
light fades within them.

$$
L \leftarrow \frac{n\,L + m\,\bar{L}_{\text{frame}}}{n + m}, \qquad n \leftarrow \min(n + m,\ 256)
$$

{{cite shaders/rt/radiance_cache_resolve.slang "const float3 radiance = count > 0u ? (old * float(count) + mean * float(a.w)) / total : mean;"}}

{{cite shaders/rt/radiance_cache_resolve.slang "count = min(count + a.w, rc.maxSamples);"}}

A slot that got no sample this frame ages by one; past 64 frames unseen its key and radiance
are cleared and the slot is free. Every slot's frame sums are zeroed for the next update.
`resolved.w` packs the count in its low 16 bits and the age in its high 16.

{{cite shaders/rt/radiance_cache_resolve.slang "if (age > rc.staleFrames)"}}

{{cite shaders/rt/radiance_cache_resolve.slang "accum[slot] = uint4(0u);"}}

{{cite ohao/render/rt/radiance_cache.hpp "static constexpr uint32_t kStaleFrames = 64;             // frames without a sample before a cell is freed"}}

A lookup finds the slot and returns its radiance only if its count is nonzero: a cell inserted
this frame and not yet resolved reads as a miss.

{{cite shaders/rt/radiance_cache.slang "if ((r.w & 0xFFFFu) == 0u) return false;"}}

## The update trace

`cacheUpdate` is a second raygen entry in `path_tracer.slang`. It launches one thread a 5x5
tile, and each traces from one pixel of its tile, chosen by a hash of the frame and the tile,
so a different pixel each frame.

{{cite shaders/rt/path_tracer.slang "const uint o = pcg(pc.params.z * 0x9E3779B9u ^ pcg(tile.x ^ pcg(tile.y))) % 25u;"}}

The path runs up to 3 bounces with the same light, environment and BSDF sampling as the
frame's raygen, and past them ends in the cache. It remembers the slot and the throughput of
its last 4 vertices; every contribution found later on the path (a light's next-event sample,
the environment, an emitter or the sky the bounce meets, emission, the cache at its end) is
added to each remembered vertex's cell divided by the throughput that reached that vertex.

{{cite shaders/rt/path_tracer.slang "static const uint kCacheUpdateBounces = 3u;"}}

{{cite shaders/rt/path_tracer.slang "static const uint kCacheUpdateVertices = 4u;"}}

{{cite shaders/rt/path_tracer.slang "cacheAdd(cacheAccum, v[k].slot, float3(t.x > 0.0 ? c.x / t.x : 0.0, t.y > 0.0 ? c.y / t.y : 0.0,"}}

A contribution $c$ reaching the camera through throughput $T_k$ at vertex $k$ is
$T_k \cdot L_r(x_k)$ for the radiance $L_r$ that vertex reflects, so each cell receives an
estimate of the light its surface reflects toward the path, one sample a vertex.

$$
L_r(x_k) \approx \frac{c}{T_k}
$$

A vertex's own emission is added before its slot is recorded, so it reaches the earlier
vertices only: a cell holds reflected light without its surface's emission, and a path that
ends in it, having already added that emission at the hit, counts it once.

{{cite shaders/rt/path_tracer.slang "cacheContribute(verts, count, throughput * hit.emission);"}}

{{cite shaders/rt/path_tracer.slang "verts[count].slot = cacheInsert(cacheKeys, cacheCell(hit.position, hit.geoNormal, camPos));"}}

Because the update itself ends in the cache, light from bounce 4 onward arrives through cells
filled by earlier frames; over frames the cells carry every bounce.

## Where a path ends in the cache

The frame's raygen reads the cache only with the cache flag set (the realtime profile), only at
its second vertex or later, and only when the segment that reached the hit is at least a voxel
long and the lobe that drew it spreads over the voxel: a diffuse lobe, or roughness² × length at
least a voxel. A sharp reflection keeps tracing, because a voxel's average would blur it.

{{cite shaders/rt/path_tracer.slang "if (!stop && depth > 0u && (flags & kFlagCache) != 0u)"}}

{{cite shaders/rt/path_tracer.slang "if (hit.t >= voxel && (lastLobe == 1u || lastRough * lastRough * hit.t >= voxel))"}}

The query point is jittered across the voxel's face in the surface's tangent plane (Binder et
al. 2019), so neighbouring pixels read neighbouring cells and no voxel shows as a block. A hit
adds the cell's radiance times the throughput and ends the path; a miss (no slot, or no
resolved sample yet) traces on.

{{cite shaders/rt/path_tracer.slang "const float2 j = (float2(float(h & 0xFFFFu), float(h >> 16u)) / 65536.0 - 0.5) * voxel;"}}

{{cite shaders/rt/path_tracer.slang "const float3 c = throughput * cached;"}}

## The host

`render::RadianceCache` owns three device-local buffers of `capacity` entries (keys 4 bytes,
sums and resolved 16 bytes each): 2^21 entries, 72 MiB, for the realtime profile, and a single
entry otherwise, so the path tracer's bindings 39-41 are always valid.

{{cite ohao/render/rt/radiance_cache.hpp "static constexpr uint32_t kDefaultCapacity = 1u << 21;   // 2M entries, 72 MiB"}}

{{cite ohao/render/rt/rt_profile_renderer.hpp "m_pathTracer.setRadianceCacheCapacity(m_settings.radianceCache ? render::RadianceCache::kDefaultCapacity : 1u);"}}

{{cite shaders/rt/path_tracer.slang "[[vk::binding(39, 0)]] RWStructuredBuffer<uint> cacheKeys;"}}

With the setting on and a full-size cache, a frame records the cache's begin barrier (the
previous frame's cache work before this frame's, or zero-fills if a clear is pending), the
update trace over (w+4)/5 x (h+4)/5 tiles, the resolve, and then the frame's trace. The resolve
binds its own pipeline layout and push constants, which replace the path tracer's, so those
are pushed again before the frame's trace.

{{cite ohao/render/rt/path_tracer_render.cpp "const bool cacheOn = m_renderSettings.radianceCache && m_radianceCache.capacity() > 1u;"}}

{{cite ohao/render/rt/path_tracer_render.cpp "(m_width + 4u) / 5u, (m_height + 4u) / 5u, 1);"}}

{{cite ohao/render/rt/path_tracer_render.cpp "// Push constants are shared by the bind points: the resolve's replaced the trace's."}}

`resetAccumulation` clears the cache (the fill runs at the next frame's begin), so a capture
after a reset starts from an empty cache in any host and repeats.

{{cite ohao/render/rt/path_tracer.cpp "m_radianceCache.clear();   // a capture after a reset starts from an empty cache in any host"}}

{{cite ohao/render/rt/radiance_cache.cpp "for (VkBuffer b : {m_keys, m_accum, m_resolved}) vkCmdFillBuffer(cmd, b, 0, VK_WHOLE_SIZE, 0u);"}}

## Measured

Measured with `tests/renderer/check_material.py` and captures on this machine: the realtime
profile's closed box (Le = 1, ρ = 0.5, exactly 2 with every bounce) reaches 1.972 at 256 frames,
within 3% of 2 at 2 traced bounces, and two runs give the same bits. Realtime Cornell at 256
frames is within 0.3% of the offline reference overall, its worst 4x3 cell 1.4%. A realtime
frame took 1.20 ms of GPU time against 3.62 ms for the offline profile at 640x360.

:::key
The cache trades a frame's long paths for short ones plus a lookup: its error is the voxel's
average and the clamp, and the offline profile, which never reads it, is what that error is
measured against.
:::

## Contracts

- A cell holds reflected radiance without its own surface's emission; a path that ends in it must already have added the hit's emission.
- Every write to the cache's sums is an integer `InterlockedAdd`; a float accumulation would make captures order-dependent.
- The update raygen, the resolve and the frame's raygen are recorded in that order every cached frame, with the push constants pushed again after the resolve.
- `resetAccumulation` clears the cache; a capture after a reset must not see cells from before it.
- The offline profile never sets the cache flag: it stays the unbiased reference.
- Bindings 39-41 are always bound; a profile without the cache gets a one-entry cache, not a missing buffer.
