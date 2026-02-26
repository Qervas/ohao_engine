# OHAO Engine — Strategic Roadmap

## The Thesis

Game engines are the last major developer tool category that hasn't been rebuilt for AI. Unity (2005) and Unreal (1998) are architecturally incapable of being AI-native — their codebases are millions of lines of assumptions about human operators clicking through GUIs. OHAO is a from-scratch Vulkan game engine where the AI agent is the primary user and natural language is the primary interface.

**The bet:** The next great game engine won't be the one with the most features. It will be the one that makes features accessible through intent.

---

## Competitive Landscape

### Incumbents

| Engine | AI Strategy | Weakness |
|--------|------------|----------|
| **Unity** | "Unity AI" beta (GDC March 2026) — prompt casual games into existence. Sunsetting Muse/Sentis, rebuilding as unified AI. | 20-year legacy architecture. AI is a wrapper, not native. Limited to "casual games." Can't expose rendering internals to AI — too much legacy abstraction. |
| **Unreal** | Procedural generation, AI Blueprint logic, PCG framework. | C++ complexity is the barrier, not missing AI features. AI can't reason about 50M lines of code. Enterprise-scale legacy. |
| **Godot** | Community-driven, no official AI strategy. Open source. | Small team, no AI roadmap, renderer is not AAA-grade. |

### Funded AI-Native Startups

| Company | Funding | What They Do | Why They're Not OHAO |
|---------|---------|-------------|---------------------|
| **Sett** | $27M Series A (May 2025) | AI agents for mobile game marketing — generates ad creative and in-game assets for UA. Works with Zynga, Playtika. | Marketing tool, not an engine. No renderer, no physics, no gameplay. |
| **Iconic** | $13M Seed (Dec 2025) | AI-native game engine for voice-driven on-device games. Backed by Google AI Futures Fund, Northzone. Founded by engine builders + AI researchers. | **Direct competitor.** But focused on voice-driven mobile experiences, not AAA. |
| **Rosebud AI** | $15M Series A (2025) | Web-based platform — text prompt to playable 2D/3D games via JavaScript. Casual/browser games. | JavaScript runtime, browser-only, casual quality. No custom renderer. No physics engine. Toys, not tools. |
| **Scenario** | $6M+ | AI-generated 2D game assets (textures, sprites, concepts). Style-consistent. | Asset generator only, not an engine. No code generation, no gameplay. |
| **Inworld AI** | $120M+ | AI NPCs with personality, dialogue, behavior. Integrates with Unity/Unreal. | Runtime AI only. No engine, no asset pipeline, no rendering. |
| **Promethean AI** | $300K → **Acquired by Sony (Oct 2024)** | 3D environment suggestion/placement. Cuts level design time 80%. | Validated the space but was middleware, not an engine. Sony bought the tech. |

### The Gap in the Market

Nobody is building an AI-native engine that produces **AAA-quality output**. Rosebud makes casual browser games. Unity AI targets casual mobile. Sett does marketing. Iconic does voice games. Inworld does NPCs.

OHAO has a custom Vulkan deferred renderer with PBR, cascaded shadows, compute-based culling, IBL, GPU particles, and a full physics engine with GJK/EPA. That's AAA pipeline foundation + AI-native interface. Nobody else has both.

### Market Data

- **$1.8B** in VC invested in Gaming x AI (2024-2025)
- **530+** startups in Gaming x AI, but no AI-native AAA engine
- **79%** of gamers more likely to buy games with AI NPCs; **81%** would pay a premium
- Gaming VC hit decade low in 2025 ($627M in H1) — but **AI game dev tools** are the exception
- Series A median valuation: **$47.9M** (2025). Typical raise: $10-15M
- **Roblox creator payouts: $1B+/year** — validates the platform/marketplace model at scale
- Promethean AI (3D environment AI) acquired by **Sony** after raising only $300K — validates the tech thesis even at tiny scale
- GitHub Copilot: devs complete tasks **55% faster**, generates **46%** of code — AI-assisted dev is proven

### AI Asset Generation (State of the Art, 2026)

What AI can actually produce today, game-ready or near-ready:

| Asset Type | Best Tools | Quality | Speed vs Manual |
|-----------|-----------|---------|----------------|
| 3D Models | Tripo AI, Meshy, 3D AI Studio | 70% game-ready (needs cleanup) | 1 min vs 4-8 hrs |
| PBR Textures | Tripo AI Texturing, Stability | 90% game-ready | Seconds |
| Rigging | Tripo AI one-click universal | 80% (humanoid/animal) | Instant |
| Animation | Meshy 500+ motion library | 60% (static poses need work) | Minutes |
| 2D/Sprites | Unity Generators, Scenario | 95% game-ready | Seconds |
| Sound Effects | Unity AI Sound, ElevenLabs | 85% game-ready | Seconds |

**Integration opportunity:** Wire Tripo/Meshy APIs directly into the engine. "Describe a 3D model" → AI generates it → auto-imported with PBR materials → placed in scene. Nobody has this end-to-end.

---

## Current State (Honest Assessment)

### What's Strong
- **Rendering:** Full PBR pipeline, 13 material presets, deferred + forward, CSM shadows, 8 post-processing effects, IBL with environment maps, GPU frustum culling, bindless textures
- **Physics:** GJK/EPA narrow phase, BVH broad phase, 6 shape types, force system with presets (gravity, drag, spring, field forces), multithreaded simulation
- **Animation:** Skeletal animation, GPU skinning, glTF extraction
- **Particles:** GPU compute-based, 4 types, directed emission
- **Integration:** Clean Godot GDExtension, 80+ GDScript API methods, helper singletons, declarative scene building
- **AI Interface:** 6 Claude Code skills, natural language game creation, reference FPS framework

### What's Missing (Blocks Credibility)
- **Visuals:** Everything is flat colors. Material system supports textures but nothing actually uses them in the AI pipeline. Investors see colored cubes.
- **Audio:** Zero. No system at all.
- **Networking:** Zero.
- **Animation:** No blending, no IK, no state machines. One animation plays at a time.
- **Physics:** No joints/constraints, no ragdoll (skeleton exists but no constraints), no character controller
- **Advanced rendering:** No real-time GI (only IBL), no terrain, no water, no decals, no ray tracing
- **Tooling:** No material editor, no particle editor, no animation editor, no undo/redo
- **Asset pipeline:** No FBX, no USD. glTF + OBJ only.

---

## Strategic Priorities

The roadmap is organized by what moves the needle for **fundraising** and **competitive moat**, not by what's technically interesting.

### Priority 1: Visual credibility (investors judge with eyes)
### Priority 2: AI moat depth (what Unity/Unreal can't copy)
### Priority 3: Core engine completeness (table stakes for adoption)
### Priority 4: Platform & ecosystem (what makes it a business)

---

## Phase 0: The Fundable Demo (Weeks 1-6)

**Goal:** A 90-second video that makes VCs say "holy shit." Show a game being built in real-time through natural language, with visual quality that doesn't look like a prototype.

### 0.1 Texture Pipeline Activation
The PBR material system already supports texture maps (albedo, normal, metallic, roughness, AO, emissive). Wire it up end-to-end:
- Load textures from disk in the AI pipeline (not just hardcoded)
- `/new-scene` and `/new-game` skills assign textures to materials
- Bundle 20-30 free PBR texture sets (stone, metal, wood, concrete, grass, etc.)
- Materials go from `Color(0.3, 0.3, 0.35)` to actual stone/metal/wood

**Impact:** Scenes go from programmer-art to believable. Single biggest visual upgrade.

### 0.2 AI Asset Integration
- Integrate with image generation API (Stability AI / DALL-E / local SD) for on-demand texture generation
- `/atmosphere foggy stone dungeon` → AI generates stone wall textures + applies them
- `/new-scene` can describe materials: `"material": "rusty metal"` → generates or selects matching PBR textures

**Impact:** This is the demo moment. "Describe a material, watch it appear." Nobody else does this with a real renderer.

### 0.3 Demo Scenario
Script the demo:
1. Start with empty viewport
2. `/new-game dark fantasy dungeon crawler` — game scaffolds itself (5 seconds)
3. `/atmosphere dark torchlit dungeon` — lighting and fog configure (2 seconds)
4. `/add-enemy armored skeleton warrior` — enemy appears with behavior (3 seconds)
5. `/add-weapon flaming sword` — weapon added to player (2 seconds)
6. Play the game for 30 seconds — it works, it looks good, it's fun
7. `/atmosphere bright outdoor meadow` — entire mood changes in real-time (2 seconds)

Total: 90 seconds from nothing to playable game.

### 0.4 Audio Foundation (Minimum Viable)
- Integrate miniaudio or SoLoud (single-header C libraries)
- 3D positional audio, basic effects (reverb, volume falloff)
- `/new-game` skills generate audio setup (footsteps, gunshots, ambient)
- Bundle 20-30 free sound effects

**Impact:** Games feel real. Silent games feel like demos.

### Deliverable
- Video demo (90 seconds)
- Playable build
- Pitch deck with technical moat thesis

---

## Phase 1: Rendering Credibility (Weeks 7-14)

**Goal:** Output quality that makes people forget this isn't Unreal.

### 1.1 Real-Time Global Illumination
- Screen-Space Global Illumination (SSGI) — compute shader, traces screen-space rays for indirect lighting
- Light probes for static GI (bake + interpolate)
- This takes scenes from "lit" to "believably lit"

### 1.2 Terrain System
- Heightmap-based terrain with GPU tessellation
- Multi-texture splatting (grass, rock, dirt, snow based on height/slope)
- LOD with distance-based tessellation reduction
- AI-addressable: `/new-scene open world grassland` generates terrain

### 1.3 Water Rendering
- Planar reflections + refraction
- Animated normal maps for waves
- Depth-based coloring (shallow = clear, deep = dark)
- Foam at shore/object intersection
- AI-addressable: `"objects": [{"type": "water", "pos": ..., "scale": ...}]`

### 1.4 Decal System
- Projected decals for bullet holes, blood, footprints
- Deferred decals (project into G-buffer)
- AI-addressable: weapon impacts auto-spawn decals

### 1.5 Vegetation & Foliage
- Billboard grass with wind animation
- Instanced rendering for thousands of plants
- GPU-driven placement from density maps

### Deliverable
- Side-by-side comparison: OHAO scene vs Unreal scene (same concept)
- Technical blog post on the rendering pipeline

---

## Phase 2: Deep AI Moat (Weeks 7-18, parallel with Phase 1)

**Goal:** Build capabilities that are architecturally impossible for Unity/Unreal to replicate without rebuilding from scratch.

### 2.1 AI Material Architect
The engine's material system has 7 texture slots + 15 scalar properties. Expose ALL of them to AI:
- Describe a material in words → AI generates/selects textures + sets PBR properties
- "Worn copper with green patina" → copper albedo, green-tinted AO, high metallic, medium roughness
- "Glowing alien slime" → green emissive, high transmission, low roughness
- Material database that grows (AI-generated materials cached for reuse)

**Why Unity can't copy:** Their material system is shader graphs designed for human clicking. OHAO's is a flat property struct designed for AI parameter filling.

### 2.2 AI Level Architect
Go beyond "place objects at coordinates":
- Architectural grammar system: rooms, corridors, doorways, stairs with connectivity rules
- AI describes spatial intent → engine generates geometry with proper connectivity
- "L-shaped corridor connecting to a large circular arena" → actual geometry, not just boxes
- Procedural detail placement (props, debris, decorations based on theme)

**Why Unity can't copy:** Their scene is a manual object hierarchy. OHAO can have a native spatial grammar the AI speaks.

### 2.3 AI Behavior Architect
Go beyond scripted state machines:
- LLM-driven NPC decision making at runtime (local models via llama.cpp)
- Behavior trees generated from natural language descriptions
- "This enemy is cowardly, attacks from range, flees when hurt" → generates behavior tree, not just stats
- Dynamic difficulty: AI observes player performance, adjusts spawning/damage/item drops

**Why Unity can't copy:** Their AI is Bolt/NavMesh designed for manual configuration. OHAO can make behavior generation native.

### 2.4 AI Playtester
The engine plays its own games:
- Automated gameplay testing (AI agent plays the game, reports bugs)
- Balance analysis: "Enemy X is too easy, weapon Y is overpowered"
- Coverage mapping: which areas of the level were explored, which weren't
- Performance profiling: where does framerate drop, what's the GPU bottleneck

**Why this matters for fundraising:** "Our engine doesn't just build games, it tests them."

### 2.5 Conversational Iteration Loop
The killer feature isn't generation — it's iteration:
- Play the game → pause → "make the enemies faster and the lighting darker" → resume
- Real-time parameter tweaking through conversation
- A/B testing: "show me two versions — one horror, one action" → side by side

---

## Phase 3: Engine Completeness (Weeks 15-26)

**Goal:** Close every gap that makes someone say "I can't ship a real game with this."

### 3.1 Audio System
- Spatial 3D audio with HRTF
- Sound categories: ambient, effects, music, UI, voice
- Reverb zones (cave reverb, outdoor reverb, room reverb)
- Audio occlusion (sounds muffled through walls)
- AI integration: `/atmosphere` configures ambient audio, `/add-weapon` assigns weapon sounds
- Library: miniaudio or OpenAL Soft

### 3.2 Animation System Upgrade
- Animation blending (crossfade between clips)
- Animation state machines (idle → walk → run → jump with transitions)
- Additive animations (breathing on top of walking)
- Inverse Kinematics — at minimum, two-bone IK for feet placement and hand targeting
- Root motion support
- Animation events (trigger particles/sounds at specific frames)
- Procedural animation helpers (look-at, aim-at)

### 3.3 Physics Upgrade
- Joint constraints: hinge, ball-socket, fixed, slider, distance
- Ragdoll system: skeleton-to-rigidbody mapping with joint limits
- Character controller: kinematic capsule with ground detection, slopes, steps
- Trigger volumes (enter/exit callbacks)
- Physics layers/masks for collision filtering
- Vehicle physics (basic: wheel colliders, suspension, steering)

### 3.4 Networking Foundation
- Client-server architecture using ENet or GameNetworkingSockets
- State synchronization: snapshot interpolation
- Input prediction and server reconciliation
- Entity replication (auto-sync marked properties)
- Lobby system and matchmaking scaffold
- AI integration: `/new-game multiplayer arena shooter` generates networked game

### 3.5 Scene System Upgrade
- Full scene serialization (save/load)
- Prefab system (reusable entity templates)
- Scene streaming (load/unload chunks)
- Undo/redo command system (the skeleton exists, complete it)
- Entity tags and layers

### 3.6 Asset Pipeline Expansion
- FBX import (via Assimp or ufbx)
- USD support (basic)
- Asset hot-reload (modify file → engine picks up changes)
- Asset compression (texture BC7/ASTC, mesh quantization)
- Asset database with metadata

---

## Phase 3.5: Next-Gen AI Moat (Weeks 22-30, parallel with Phase 3)

**Goal:** Build capabilities that put OHAO 2-3 years ahead of anything Unity/Unreal can bolt on.

### 3.5.1 Neural Material System
- Materials learned from reference photos (no manual PBR authoring)
- User photographs a wall → AI extracts albedo, normal, roughness, metallic, AO maps
- Requires: image-to-PBR model (fine-tuned diffusion model or Tripo texture API)
- The engine's flat PBR property struct is perfect for this — 7 texture slots + 15 scalars, all AI-fillable
- **Unity can't copy:** Their material system is shader graphs designed for node-clicking. OHAO's is a data struct designed for AI.

### 3.5.2 Semantic Scene Graph
- Every object in the scene annotated with meaning: "this is a chair," "this is a combat zone," "this corridor connects room A to room B"
- AI uses semantics for: context-aware suggestions, design validation ("this combat arena has no cover"), auto-balancing
- Build on the existing Actor-Component system — add SemanticComponent with tags, spatial relationships, gameplay role
- **Unity can't copy:** Their scene graph is a raw transform hierarchy. Adding semantics would break backwards compatibility.

### 3.5.3 Continuous Learning Loop
- Engine captures gameplay telemetry during playtesting
- AI analyzes: "level 3 is 40% harder than level 2," "weapon X is overpowered," "players get lost at junction Y"
- Genetic algorithm optimizes: enemy placement, loot tables, difficulty curves
- Publishes balance reports automatically
- **This is the pitch line:** "Our engine doesn't just build games. It tests them, balances them, and improves them."

### 3.5.4 Runtime LLM NPCs
- Integrate llama.cpp for local inference (no cloud dependency)
- NPCs with personality, long-term memory, emotional state, dynamic dialogue
- Player demand is massive: 79% of gamers want AI NPCs, 81% would pay premium
- Behavior emerges from personality description, not scripted state machines
- "This shopkeeper is grumpy, hates adventurers, but has a soft spot for cats" → NPC acts accordingly

---

## Phase 4: Platform & Business (Weeks 20-32, overlapping)

### 4.1 Web Playground
- WebGPU port of the renderer (or video-streamed preview)
- Browser-based: type a game description → see it built → play it
- No install required — this is the viral growth mechanism
- Shareable links: "here's the game I made in 2 minutes"

### 4.2 Export Targets
- Windows executable (already works via Godot)
- WebGL/WebGPU export
- Linux export
- macOS export
- Mobile (stretch goal — Vulkan on Android, Metal on iOS)

### 4.3 Asset Marketplace
- Community-created materials, prefabs, enemy types, weapon configs
- AI-generated asset packs (textures, sounds, models)
- Revenue share model

### 4.4 Documentation & Community
- Interactive tutorial: "Make your first game in 5 minutes"
- API documentation auto-generated from skills
- Discord community
- Example game library (10+ genres)
- Video content: "Building [game] in 60 seconds with OHAO"

---

## Funding Strategy

### Seed Round ($3-5M)

**When:** After Phase 0 demo is complete (Week 6-8)

**Comparable raises:** Iconic ($13M seed), Rosebud ($15M Series A), Promethean (acquired by Sony pre-Series A). The space is fundable.

**Pitch:**
- Problem: Game engines are the last un-disrupted developer tool. $200B gaming industry, tools built 20 years ago.
- Solution: AI-native game engine. Natural language → playable game. Custom Vulkan renderer (not a wrapper). From-scratch architecture designed for AI agents.
- Demo: 90-second video of game being built through conversation.
- Differentiation: "Not Rosebud (browser toys). Not Inworld (middleware). Not Unity AI (bolt-on). Full engine, AI-native from byte zero."
- Team: [you + hires]
- Market: 2.5M+ game developers worldwide. $6B game engine market. 530+ startups in Gaming x AI but no one doing AI-native AAA.
- Ask: $3-5M to complete rendering pipeline, AI asset system, and audio. Hire 3-4 engineers.

**Target VCs:**
- BITKRAFT Ventures (gaming-focused, all stages, most active gaming VC)
- Makers Fund (100+ gaming investments, creativity + tech thesis)
- Northzone (backed Iconic, understands AI-native engines)
- Conviction (backed Iconic, AI thesis)
- a16z Games Fund
- Google AI Futures Fund (backed Iconic)
- Play Ventures (gaming infrastructure focus)
- VGames (game entrepreneur fund)

**Metrics that matter:**
- Demo video views / virality
- Games created per day (once playground exists)
- Time-to-playable-game (benchmark: 60 seconds)
- Developer waitlist size
- Technical benchmarks vs Unity/Unreal (render quality, build speed)

### Series A ($15-25M)

**When:** After Phase 2-3 (6-8 months post-seed)

**Milestones needed:**
- Web playground live with 10K+ users
- 1000+ games created on the platform
- 3+ genres demonstrated (FPS, RPG, platformer, puzzle, horror, strategy)
- Networking working (multiplayer games via conversation)
- At least one indie game shipped using OHAO
- Technical parity with Unity for indie-scale games

---

## Hiring Plan (Post-Seed)

| Role | Priority | Why |
|------|----------|-----|
| **Graphics Engineer** | P0 | GI, terrain, water, foliage — rendering credibility |
| **AI/ML Engineer** | P0 | Material generation, level generation, behavior generation |
| **Engine Generalist** | P1 | Audio, animation, physics joints, networking |
| **DevRel / Content** | P1 | Demos, tutorials, community, social media |

4 hires that 10x the engine. Stay small, move fast.

---

## Technical Architecture Decisions

### Why Godot as the host (keep it)
- Free, open-source, no runtime fees (unlike Unity)
- GDScript is simple enough for AI to generate fluently
- Built-in editor, UI framework, input system, scene tree — don't rebuild what works
- Export to all platforms via Godot's pipeline
- Growing community (3rd most popular engine)

### Why custom Vulkan renderer (keep it)
- Full control over rendering pipeline = AI can configure everything
- No abstraction layers between AI intent and GPU commands
- Can add features (GI, terrain, water) without fighting someone else's architecture
- Performance: direct Vulkan = optimal for real-time rendering

### Why skills over MCP (keep it)
- Zero runtime cost (skills are markdown, not server processes)
- Version controlled with the engine
- Scale with model intelligence, not server infrastructure
- Work offline, no API dependencies
- Lower token usage than MCP tool calls

### What to add: WASM/WebGPU runtime
- For the web playground: compile a lightweight renderer to WASM + WebGPU
- Or: stream rendered frames from a cloud GPU (simpler, lower quality)
- This is the viral distribution mechanism — try before you install

---

## Milestones & Decision Points

| Week | Milestone | Go/No-Go Decision |
|------|-----------|-------------------|
| 3 | Textures loading in AI pipeline, 20 PBR texture sets bundled | — |
| 5 | Audio system basic (3D positional, 30 sound effects) | — |
| 6 | **Demo video complete** | Start fundraising? |
| 8 | Seed round conversations started | Investor interest validates thesis? |
| 10 | GI prototype (SSGI or probes) | Which GI approach to ship? |
| 14 | Terrain + water + decals working | Side-by-side vs Unreal quality check |
| 16 | AI material generation integrated | Does generated quality meet bar? |
| 18 | AI level architect prototype | Evaluate spatial grammar approach |
| 20 | Animation blending + IK working | — |
| 22 | Physics joints + ragdoll + character controller | — |
| 24 | Networking prototype | Is snapshot interpolation sufficient? |
| 26 | **All core systems complete** | Ready for Series A push? |
| 28 | Web playground prototype | Cloud-streamed or WASM+WebGPU? |
| 32 | **Platform launch** | Public beta |

---

## The One-Liner

**OHAO Engine: Describe a game, play a game. The first AI-native game engine with AAA rendering.**

---

## What Makes This Win

1. **The AI doesn't use the engine. The AI IS the engine.** Every subsystem — rendering, physics, animation, audio, networking — is designed as an AI-addressable API first, human GUI second.

2. **Visual quality is the moat.** Anyone can make an AI that generates JavaScript browser games (Rosebud does). Nobody has an AI that generates games with deferred rendering, PBR materials, cascaded shadows, volumetric fog, and GPU particles. The renderer is the differentiator.

3. **From-scratch advantage.** Unity can't make their engine AI-native without rewriting it. Unreal can't simplify their architecture. OHAO starts clean.

4. **Speed compounds.** Every game built trains better skills. Every material generated joins the library. Every enemy archetype becomes a template. The engine gets smarter with use.

5. **The demo is the product.** "Watch me build a game by talking to my computer" is inherently viral. The web playground is the growth engine.
