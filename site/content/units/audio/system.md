---
module: audio
id: system
title: AudioSystem facade
standard: v2
figures: [audio-system-bus-topology]
---

## The buses that are not buses

`SoundCategory` reads like a mixer strip — SFX, Music, Ambient, each with its own
volume, each stoppable and pausable as a group. miniaudio ships a first-class
object for exactly that shape: `ma_sound_group`, a node you splice between voices
and the engine endpoint. OHAO does not use it. Both play paths pass a null group
pointer, which attaches every voice straight to the engine's endpoint:

{{cite ohao/audio/audio_system.cpp "m_engine.get(), pathStr.c_str(), flags, nullptr, nullptr, sound.get());"}}

A category is therefore two ordinary C++ things: an enum tag stored on the owning
`SoundInstance`, and a scalar folded into that one voice's gain. Nothing in the
audio graph corresponds to "music". Changing a category volume is a linear walk
over every live sound, re-multiplying the ones whose tag matches:

{{cite ohao/audio/audio_system.cpp "if (instance.category == category) {"}}

{{figure audio-system-bus-topology "Shipped signal graph (left) against the ma_sound_group topology the facade does not use (right). Conceptual — the left panel is derived from the null pGroup argument in both play paths, not a captured node-graph dump."}}

:::why
`ma_sound_group` would make `setCategoryVolume` O(1) instead of O(active sounds),
and — the part that matters more — would give the mix a real insertion point,
because miniaudio can splice effect nodes into a node graph but not into a float.
The flat design buys a facade with no hidden state: `stopCategory` is a
`std::erase_if` over a map, not a graph edit, and the entire mixer is four floats
you can print — one master and a three-element category array. The price is that
ducking music under dialogue, or putting a limiter on the SFX bus, is not a
feature you add later — it is a topology change.
:::

## Four factors, a voice cap, and no limiter

Each voice's gain is a bare four-way product, computed in C++ and pushed down
with a single `ma_sound_set_volume`:

$$v = v_b\,o\,v_c\,v_m, \qquad o = \begin{cases} 10^{-8/20} \approx 0.40 & \text{occluded} \\ 1 & \text{otherwise} \end{cases}$$

{{cite ohao/audio/audio_system.cpp "const float finalVolume = instance.baseVolume * occlusion *"}}

$v_b$ is the per-sound base volume passed to `playSound`, $o$ the occlusion cut
(`kOccludedGainDb`, −8 dB, for a sound its `SoundShape` marks as behind a wall),
$v_c$ the category scalar, $v_m$ the master. `updateSoundVolume` clamps nothing
itself; the clamp sits at the entry points, where `clampVolume` applies
$\mathrm{cl}(x) = \min(\max(x,0),1)$ to whatever the caller supplies — in both
play paths, in `setSoundVolume`, in `setCategoryVolume` and in
`setMasterVolume`. Every factor is therefore already in $[0,1]$ before the
multiply, and so is the product: a voice can be attenuated, never amplified.
That ceiling is per voice, not per mix — the endpoint sums N voices with nothing
after it, so a dozen simultaneous unity-gain effects clip the output while every
individual gain is legally "at most 1".

What bounds N is a voice cap, not a limiter. `setVoiceLimit` (0, the default,
is none; the window host sets 24) caps the SFX sounds playing at once. A new one
over the cap takes the place of the quietest at the listener, or is dropped when
it would be the quietest itself, and either way one sound is counted in
`voicesDropped`:

{{cite ohao/audio/audio_system.cpp "if (quietest == m_sounds.end() || audible <= quietestAudible) return false;   // the new one is the quietest"}}

Twenty-four unity-gain voices can still clip the sum; the cap only stops the
count growing without bound in a firefight.

The clamp is the only shaping in the chain, and it is linear amplitude rather
than perceptual: a master slider at 0.5 is −6 dB, which reads as "somewhat
quieter", not "half as loud". A UI wired straight to this value is top-heavy, and
no curve is applied anywhere to fix that.

## Spatialisation is opt-in because the default is on

miniaudio enables spatialisation on every `ma_sound` by default, so the
non-positional path has to switch it back off — otherwise a UI click would be
panned and attenuated according to wherever the listener happens to be standing:

{{cite ohao/audio/audio_system.cpp "ma_sound_set_spatialization_enabled(sound.get(), MA_FALSE);"}}

That line is load-bearing, not defensive. The positional path sets the same flag
to `MA_TRUE` — already the default — and then writes the distance model out
longhand, from the `SoundShape` the caller passes:

{{cite ohao/audio/audio_system.cpp "ma_sound_set_min_distance(sound.get(), shape.minDistance);"}}
{{cite ohao/audio/audio_system.cpp "ma_sound_set_max_distance(sound.get(), shape.maxDistance);"}}
{{cite ohao/audio/audio_system.cpp "ma_sound_set_rolloff(sound.get(), shape.rolloff);"}}
{{cite ohao/audio/audio_system.cpp "ma_sound_set_attenuation_model(sound.get(), ma_attenuation_model_inverse);"}}

A default `SoundShape` is 1 m, 50 m and rolloff 1. Against
`ma_spatializer_config_init`, which already selects the inverse model, a minimum
distance of 1 and rolloff 1, only the maximum changes miniaudio's behaviour (its
default is `MA_FLT_MAX`); the rest are defaults written down. The shape makes
them a choice per sound: the window host gives gunfire 1 to 60 m and footsteps,
landings and impacts 0.5 to 20 m at rolloff 1.5 (`runtime/game_audio.cpp`). A
shape marked `occluded` also routes the sound through a low-pass node at
`kOccludedLowPassHz` (1,200 Hz) on its way to the endpoint, beside the −8 dB cut
in the gain above — the one place a node now sits between a voice and the mix.

miniaudio's inverse model (OpenAL's `AL_INVERSE_DISTANCE_CLAMPED`) computes a
scalar gain from the listener-to-source distance $d$:

$$g(d) = \frac{d_{\min}}{d_{\min} + \rho\,\big(\mathrm{clamp}(d,\,d_{\min},\,d_{\max}) - d_{\min}\big)}$$

With the default shape, $d_{\min}=1$, $d_{\max}=50$ world units and rolloff
$\rho=1$, the expression collapses to $g(d) = 1/\mathrm{clamp}(d,1,50)$: flat
unity inside one unit, honest inverse-distance out to fifty, then a hard floor of
$1/50 = 0.02$, roughly −34 dB. The facade computes the same curve itself
(`distanceGain`) to rank voices for the cap.

The floor is the sharp edge. A looping ambient a kilometre away is neither culled
nor silenced; it is held at $g(d_{\max})$, still decoded, still mixed, forever.
Culling by distance would be the caller's job.

## Where the mutex stops

`m_mutex` guards `m_sounds` and nothing else. That is a narrower promise than the
class's shape implies, and it shows in four places.

The handle counter is bumped outside the lock — both play paths end in
`start()`, which calls `nextHandle()` before entering the critical section, and
the counter is a plain non-atomic read-modify-write:

{{cite ohao/audio/audio_system.cpp "return m_nextHandle++;"}}

Two threads playing concurrently can be issued the same handle, and the second
insertion then destroys the first sound through the map.

The mixer scalars are written outside the lock. `setMasterVolume` stores the
clamped float first and only then locks to walk the map; `setCategoryVolume` has
the same shape:

{{cite ohao/audio/audio_system.cpp "m_masterVolume = clampVolume(volume);"}}

The reads are not symmetric, and the asymmetry does not save it.
`updateSoundVolume` reads both scalars with the mutex held — all four of its call
sites are inside locked regions — but a reader holding a lock the writer never
takes is still racing. The two public getters do not even have that:
`getMasterVolume` is an inline return in the header, and `getCategoryVolume` is
an out-of-line read behind a category-validity branch, neither of them locked:

{{cite ohao/audio/audio_system.cpp "return m_categoryVolumes"}}

Torn float reads are not a real hazard on the targets OHAO builds for, but these
are races and a sanitiser will say so.

The third one bites. Both play paths capture a raw `ma_sound*`, move the owning
instance into the map, release the lock, and only then start the sound. Nothing
in the code says why the start is outside the critical section, and there is
little to buy: `ma_sound_start` on a freshly initialised sound takes no lock of
its own — it reads a few atomics and exchanges the node's state. What the ordering
costs is a window in which a raw pointer to a map-owned object is live while the
map is unlocked, and `stopAll` needs no handle to run:

{{cite ohao/audio/audio_system.cpp "void AudioSystem::stopAll() {"}}

and clearing the map runs `MaSoundDeleter`, which uninitialises and frees the
object that raw pointer still refers to:

{{cite ohao/audio/audio_system.cpp "ma_sound_uninit(sound);"}}

A `stopAll` or `stopCategory` landing in that window starts a freed `ma_sound`.
`stopSound` cannot reach it — the handle has not been returned yet — but the
category-wide and global stops can, and the mutex exists precisely to promise
they cannot.

The fourth is the widest, and it is not a window but a whole function.
`shutdown()` takes no lock at all: it clears `m_sounds`, destroys the engine and
clears `m_initialized`, and the destructor calls it.

{{cite ohao/audio/audio_system.cpp "m_engine.reset(); // MaEngineDeleter uninits the engine"}}

A `playSound`, `update` or `stopAll` running concurrently with teardown is an
unguarded mutation of exactly the container the mutex is supposed to own, racing
a thread that never takes the mutex at all. `m_initialized` is the same story one
size down: written there, read without the lock by `update()`'s early-out and
by the inline `isInitialized()`. Single-threaded teardown is the only regime in
which this is safe, and nothing in the class enforces it.

:::key
There is no audio graph here beyond a flat fan-in to one endpoint, with a
low-pass node in front of an occluded voice. Every "bus" behaviour — category
volume, category stop, master volume — is C++ iterating a map and calling
`ma_sound_set_volume` per voice; the voice cap bounds how many there are. The mutex covers that map while
it is being walked; it does not cover the writes to the mixer scalars, the handle
counter, or `shutdown()`.
:::

## One translation unit pays for miniaudio

The header forward-declares miniaudio's two types as incomplete structs:

{{cite ohao/audio/audio_system.hpp "struct ma_engine;"}}

and owns them through `std::unique_ptr` with stateless custom deleters whose
`operator()` is declared in the header and defined in the `.cpp`, where the types
are complete. A `unique_ptr` with a custom deleter never needs the pointee to be
complete — it only needs to call the deleter — so no translation unit that merely
uses `AudioSystem` pays for miniaudio's single-header build. `miniaudio.h` is
included in exactly one file in the tree, and that file is the one that asks for
the implementation to be emitted:

{{cite ohao/audio/audio_system.cpp "#define MINIAUDIO_IMPLEMENTATION"}}

The library semantics this page leans on — default-on spatialisation, the
spatializer's default model and distances, non-rewinding stop — belong to a
particular version, and the fetch names one rather than a moving ref:

{{cite external/cmake/miniaudio.cmake "URL https://github.com/mackron/miniaudio/archive/refs/tags/0.11.21.zip"}}

That pin only binds on the branch that fetches. The module looks first for a
`miniaudio.h` already sitting in the build tree and, finding one, uses it with no
version check:

{{cite external/cmake/miniaudio.cmake "miniaudio: Using pre-fetched header"}}

So a stale or hand-dropped header in `build/_deps` is consumed silently, and the
URL above documents what a clean configure would download, not necessarily what
compiled.

## The reaper, and who calls it

Finished one-shots are not freed when they end; they are freed when someone asks.
`update()` is the only reaper, and its predicate is "not looping and at end":

{{cite ohao/audio/audio_system.cpp "ma_sound_at_end(instance.sound.get())"}}

Skip it and every fired one-shot stays in the map, attached to the endpoint,
being mixed as silence. The voice cap reaps too, before it counts voices, but
only when a capped SFX sound starts. The one caller that runs every frame is the
window host, which owns the engine's single `AudioSystem` (footsteps and combat
sounds play through it) and reaps after the frame's steps:

{{cite ohao/runtime/live_host.cpp "if (m_audio) m_audio->update();   // finished sounds free their voices"}}

`ohao_audio` is on the link line of every example —

{{cite examples/CMakeLists.txt "ohao_gpu_vulkan ohao_scene ohao_physics ohao_audio ohao_core -Wl,--end-group"}}

— but the examples themselves never touch `AudioSystem`; only `ohao/runtime`
does, and only in a window host with sound on (headless hosts play nothing). The
facade's own test suite pins the handle contract — a missing file returns
`INVALID_SOUND_HANDLE` rather than throwing — and skips every device-dependent
case when `initialize()` fails, so machines with no audio device still pass:

{{cite tests/audio/audio_system_tests.cpp "bool testPlayInvalidPath() {"}}

That suite still never creates a voice. The ones that do are in `runtime_tests`:
`initialize(true)` builds a mixer without a device, and `sp9_t21_voice_cap`
starts 30 positioned sounds under the 24-voice cap and checks that the 24 loudest
at the listener play and that the 6 lost are counted, while
`sp9_t21_distance_curves` checks the shapes against `distanceGain`.

## Contracts

- `m_sounds` must be cleared before `m_engine` is released — each sound holds a reference-counted handle on engine-owned decoded data, and `MaEngineDeleter` runs `ma_engine_uninit` unconditionally. `shutdown()` does it in that order and the destructor calls `shutdown()`; swapping those two lines is a use-after-free at exit.

{{cite ohao/audio/audio_system.cpp "m_sounds.clear(); // MaSoundDeleter uninits each sound"}}

- `m_categoryVolumes` is a hard-coded `std::array<float, 3>`, not sized from `SoundCategory::Count`. Add a fourth category, teach `isValidSoundCategory` to accept it, and `setCategoryVolume` writes out of bounds — silently, because the index comes from the enum and nothing checks it against the array.

{{cite ohao/audio/audio_system.hpp "std::array<float, 3> m_categoryVolumes"}}

- `pauseSound` is `ma_sound_stop`, which in miniaudio does not rewind, so `resumeSound`'s `ma_sound_start` continues from the cursor. `stopSound` is different in kind: it erases the instance, so the handle dies with the sound.

- Handles are never recycled and 0 is reserved, so a stale handle is a lookup miss and every setter no-ops. "Reaped", "stopped" and "never existed" are indistinguishable from outside — `isPlaying` returns false for all three, and for a paused sound as well.

- One listener, index 0, fixed at engine init. Split-screen would need `listenerCount` raised and a pinned listener index per sound; neither exists.

{{cite ohao/audio/audio_system.cpp "config.listenerCount = 1;"}}
