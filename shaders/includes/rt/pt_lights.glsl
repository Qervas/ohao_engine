#ifndef OHAO_PT_LIGHTS_GLSL
#define OHAO_PT_LIGHTS_GLSL
// The path tracer's analytic lights measured as the deferred frame measures them
// (core/deferred_lighting.frag): a point or spot light of intensity I lights a surface
// d away with I / d^2, windowed to exactly zero at the light's range. GPULight
// (render/rt/gpu_light.hpp): type 0 sphere (point), 1 directional, 2 spot, 3 area; extra2
// = (range, radius) for spheres and spots.
//
// A point, spot or area light is also a mesh (RenderScene's emitter instances, TLAS mask
// kTlasMaskEmitter): only the camera ray and a near-mirror's first specular ray (kRayMaskCamera,
// ptMirrorRoughness below) see it, as pure emission that ends
// the path. Every other ray traces kRayMaskScene, so the mesh never occludes its own light
// and no bounce adds the light a second time on top of these analytic samples.
#include "shared/layout.glsl"

// A first hit smoother than this is a mirror: its specular light comes from its specular ray,
// which alone may hit a light's mesh (kRayMaskCamera) and returns the mesh's radiance, and its
// light samples carry no GGX specular. A lobe this narrow is all but never reached by sampling a
// light's surface, so without this a mirror showed no reflection of a light at all; with it the
// reflection is the mesh, once.
const float ptMirrorRoughness = 0.05;

// The emitter's radius: a sphere keeps it in dirAndParam.w, a spot in extra2.y (its
// dirAndParam.w is the inner cone angle).
float ptLightRadius(float type, float dirParam, vec4 extra2) {
    return (type > 1.5 && type < 2.5) ? extra2.y : dirParam;
}

// The area the intensity is spread over to give the emitter's radiance Le = I / area. A
// sphere of radiance Le has radiant intensity Le * pi r^2 (its projected area), so a point
// light of intensity I has Le = I / (pi r^2); sampled uniformly over the whole sphere
// (weight cos * 4 pi r^2 / d^2) or by solid angle, that delivers I / d^2. A spot's
// estimator weights 4 pi r^2 / d^2 with no emitter cosine, so its Le = I / (4 pi r^2) is
// already I / d^2; an area light keeps its own convention. A directional light has no
// emitter: its intensity is the irradiance it gives a surface facing it, as in the deferred
// frame (weight 1), so its "area" is 1. The radius is clamped to 0.01 before this, so the area
// is never zero; it is not floored further (a floor of 0.01 m^2 dimmed every light under
// 5.6 cm, to 3% of its light at r = 0.01, against the emitter mesh's own I / (pi r^2)).
float ptEmitterArea(float type, float r) {
    if (type > 0.5 && type < 1.5) return 1.0;
    return (type < 0.5 ? 1.0 : 4.0) * 3.14159 * r * r;
}

// The deferred frame's range window, (1 - (d / range)^4)^2 clamped, for points and spots:
// exactly zero past the range, as clustered lighting needs it there.
float ptRangeWindow(float type, float d, float range) {
    if (!(type < 0.5 || (type > 1.5 && type < 2.5))) return 1.0;
    const float w = clamp(1.0 - pow(d / range, 4.0), 0.0, 1.0);
    return w * w;
}

#endif
