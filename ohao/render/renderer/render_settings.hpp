#pragma once
// Every quality knob of a renderer, one struct (system design 4.2, step 2d): render.settings reads and writes it
// by reflection (ohao/api/params.cpp registers it), VulkanRenderer applies and reads it back. Host state, never in
// scene files. The defaults are the budgeted frame and what a fresh renderer starts with; a new setting is one
// field here, one registration line, and where the renderer applies and reads it.
#include "render/display/display.hpp"

#include <cstdint>

namespace ohao::render {

enum class Quality : uint8_t { realtime, reference };
// The display curve (frontier F2, ruling F-R4): AgX by default, Khronos PBR Neutral for material review.
enum class Tonemap : uint8_t { agx, neutral };
enum class TraceResolution : uint8_t { half, full };
enum class GeometryPath : uint8_t { automatic, meshlets, instances };   // "auto", "meshlets", "instances"

struct RtDirectSettings {
    bool enabled{true};
    int32_t candidates{8};   // light points a pixel draws a frame (1-32)
    int32_t spatial{2};      // realtime: neighbours a pixel's reservoir reuses (0-8)
    TraceResolution resolution{TraceResolution::half};
};
struct RtGISettings {
    bool enabled{true};
    TraceResolution resolution{TraceResolution::half};
    int32_t rays{2};         // realtime: rays a pixel (1-32)
};
struct CsmSettings {         // the CSM fallback (without RT direct light)
    int32_t cascades{4};     // 1-4
    int32_t size{2048};      // 512-4096
};
// Presentation switches for A/B captures (sub-project 12), never in the World.
struct VfxSettings {
    bool particles{true};
    bool soft{true};         // the particles' soft fade against the scene's depth
    bool tracers{true};
    bool decals{true};
    bool smokeInGi{true};    // RT direct light and RT GI attenuate through the smoke proxies
};
struct RenderSettings {
    Quality quality{Quality::realtime};
    RtDirectSettings rtDirect;
    RtGISettings rtGI;
    bool sss{true};
    GeometryPath geometry{GeometryPath::automatic};
    bool occlusionCulling{true};
    CsmSettings csm;
    float ev100{kDefaultEv100};        // the display transform's camera (EV100, ISO 100)
    Tonemap tonemap{Tonemap::agx};
    bool autoExposure{false};          // a window meters its frames; captures always use ev100
    float exposureCompensation{0.0f};  // EV added to what auto exposure meters
    VfxSettings vfx;
};

} // namespace ohao::render
