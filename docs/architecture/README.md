# OHAO Engine Architecture

## Design Philosophy

OHAO Engine is a focused Vulkan renderer and physics simulation engine designed to integrate with Godot as a GDExtension.

### Key Decision: ImGui Removal (18,000+ lines discarded)

In early development, OHAO included a full ImGui-based editor UI. This was **intentionally removed** to:

1. **Focus on core competencies** - Renderer and physics simulation
2. **Leverage Godot's mature editor** - No need to reinvent UI/UX
3. **Reduce maintenance burden** - 18k+ lines of UI code eliminated
4. **Cleaner architecture** - Offscreen rendering enables embedding anywhere

The engine now renders to a pixel buffer, which can be displayed in any host application (Godot, Qt, native window, etc.).

---

## System Architecture

```dot
digraph OHAO_Architecture {
    rankdir=TB;
    node [shape=box, style=filled, fontname="Helvetica"];

    subgraph cluster_godot {
        label="Godot Editor";
        style=filled;
        color=lightblue;

        GodotUI [label="Godot UI\n(Scene Tree, Inspector)", fillcolor=lightskyblue];
        OhaoViewport [label="OhaoViewport\n(GDExtension Control)", fillcolor=steelblue, fontcolor=white];
        GodotScene [label="Godot 3D Scene\n(MeshInstance3D, Lights)", fillcolor=lightskyblue];
    }

    subgraph cluster_ohao {
        label="OHAO Engine (C++)";
        style=filled;
        color=lightgreen;

        OffscreenRenderer [label="OffscreenRenderer\n(Vulkan Pipeline)", fillcolor=seagreen, fontcolor=white];
        Scene [label="Scene\n(Actor/Component ECS)", fillcolor=mediumseagreen];
        PhysicsWorld [label="PhysicsWorld\n(Custom Physics)", fillcolor=mediumseagreen];
        DeferredRenderer [label="DeferredRenderer\n(AAA Quality)", fillcolor=seagreen, fontcolor=white];
    }

    subgraph cluster_vulkan {
        label="Vulkan Backend";
        style=filled;
        color=lightyellow;

        Framebuffer [label="Offscreen\nFramebuffer", fillcolor=gold];
        ShadowMap [label="Shadow Map\n2048x2048", fillcolor=gold];
        GBuffer [label="G-Buffer\n(Deferred)", fillcolor=gold];
        PixelBuffer [label="Pixel Buffer\n(CPU Readable)", fillcolor=orange];
    }

    // Connections
    GodotUI -> OhaoViewport [label="embeds"];
    GodotScene -> OhaoViewport [label="sync_from_godot()"];
    OhaoViewport -> OffscreenRenderer [label="render()"];
    OhaoViewport -> Scene [label="scene management"];

    OffscreenRenderer -> Framebuffer [label="render to"];
    OffscreenRenderer -> ShadowMap [label="shadow pass"];
    OffscreenRenderer -> DeferredRenderer [label="deferred mode"];
    DeferredRenderer -> GBuffer [label="G-Buffer pass"];

    Scene -> OffscreenRenderer [label="mesh/light data"];
    Scene -> PhysicsWorld [label="physics sync"];

    Framebuffer -> PixelBuffer [label="copy"];
    PixelBuffer -> OhaoViewport [label="getPixels()"];
    OhaoViewport -> GodotUI [label="ImageTexture"];
}
```

---

## Rendering Pipeline

```dot
digraph Rendering_Pipeline {
    rankdir=LR;
    node [shape=box, style=filled, fillcolor=lightblue, fontname="Helvetica"];

    subgraph cluster_frame {
        label="Per-Frame (Ring Buffer × 3)";
        style=dashed;

        Wait [label="Wait for\nFrame N-3", shape=ellipse, fillcolor=lightyellow];
        Update [label="Update UBOs\n(Camera, Lights)"];
        Shadow [label="Shadow Pass\n(Depth Only)", fillcolor=gray90];
        Main [label="Main Pass\n(PBR Lighting)", fillcolor=lightgreen];
        Copy [label="Copy to\nStaging Buffer"];
        Submit [label="Submit\n(Signal Fence)", shape=ellipse, fillcolor=lightyellow];
    }

    Wait -> Update -> Shadow -> Main -> Copy -> Submit;
    Submit -> Wait [label="next frame", style=dashed, constraint=false];
}
```

---

## Component System

```dot
digraph Component_System {
    rankdir=TB;
    node [shape=box, style=filled, fontname="Helvetica"];

    Actor [label="Actor", fillcolor=lightskyblue];

    subgraph cluster_components {
        label="Components";
        style=filled;
        color=lightgray;

        Transform [label="TransformComponent\n(Position, Rotation, Scale)", fillcolor=lightgreen];
        Mesh [label="MeshComponent\n(Vertices, Indices)", fillcolor=lightgreen];
        Material [label="MaterialComponent\n(PBR Properties)", fillcolor=lightgreen];
        Light [label="LightComponent\n(Dir/Point/Spot)", fillcolor=lightyellow];
        Physics [label="PhysicsComponent\n(Custom Rigid Body)", fillcolor=lightcoral];
    }

    Actor -> Transform;
    Actor -> Mesh;
    Actor -> Material;
    Actor -> Light;
    Actor -> Physics;

    Transform -> Physics [label="sync", style=dashed];
    Physics -> Transform [label="sync", style=dashed];
}
```

---

## Multi-Frame Rendering (Ring Buffer)

```dot
digraph Ring_Buffer {
    rankdir=LR;
    node [shape=record, style=filled, fontname="Helvetica"];

    Frame0 [label="{Frame 0|CMD Buffer|Fence|Camera UBO|Light UBO|Staging Buffer}", fillcolor=lightgreen];
    Frame1 [label="{Frame 1|CMD Buffer|Fence|Camera UBO|Light UBO|Staging Buffer}", fillcolor=lightyellow];
    Frame2 [label="{Frame 2|CMD Buffer|Fence|Camera UBO|Light UBO|Staging Buffer}", fillcolor=lightcoral];

    Frame0 -> Frame1 -> Frame2 -> Frame0 [style=bold];

    GPU [label="GPU Processing", shape=ellipse, fillcolor=lightblue];
    CPU [label="CPU Recording", shape=ellipse, fillcolor=lightblue];

    Frame0 -> GPU [label="executing", style=dashed];
    Frame1 -> GPU [label="queued", style=dashed];
    Frame2 -> CPU [label="recording", style=dashed];
}
```

---

## Directory Structure

```
ohao_engine/
├── src/
│   ├── engine/
│   │   ├── actor/          # Actor base class
│   │   ├── component/      # Component system
│   │   ├── scene/          # Scene management
│   │   └── asset/          # Model/texture loading
│   ├── renderer/
│   │   ├── offscreen/      # Core Vulkan renderer
│   │   ├── frame/          # Multi-frame resources
│   │   ├── passes/         # Render passes (GBuffer, CSM, etc.)
│   │   ├── camera/         # Camera system
│   │   └── components/     # Render components (Mesh, Light, Material)
│   └── physics/            # Bullet physics integration
├── shaders/
│   ├── core/               # Main shaders (forward.vert/frag, gbuffer, deferred_lighting)
│   ├── shadow/             # Shadow mapping shaders
│   ├── postprocess/        # Bloom, TAA, tonemapping
│   ├── compute/            # SSAO, SSR, volumetrics
│   └── includes/           # Shared GLSL (BRDF, lighting, shadow PCF)
├── godot_editor/
│   ├── src/                # GDExtension source (OhaoViewport, OhaoPhysicsBody)
│   └── project/            # Godot project with OHAO plugin
└── docs/
    ├── architecture/       # This document
    └── bugs_solved/        # Bug fix history
```

---

## Rendering Modes

| Mode | Description | Use Case |
|------|-------------|----------|
| **Forward** | Single-pass PBR with 8-light limit | Simple scenes, lower GPU |
| **Deferred** | G-Buffer + light culling, unlimited lights | Complex scenes, AAA quality |

---

## Future Roadmap

1. **GPU-Driven Rendering** - Indirect draw, GPU culling
2. **Mesh Shaders** - When MoltenVK supports them
3. **Ray Tracing** - Hybrid RT for reflections/GI
4. **Asset Pipeline** - glTF 2.0 with material import
