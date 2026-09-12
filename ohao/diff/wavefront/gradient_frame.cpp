// The assembly the header argues for. Moved verbatim out of
// tests/diff/context/probes_gradient.cpp, reasoning and all.
#include "diff/wavefront/gradient_frame.hpp"

namespace ohao::diff {

GenerateCameraPush generatePush(const GradientFrame& frame, std::uint32_t capacity) {
    GenerateCameraPush push{};
    for (int i = 0; i < 3; ++i) {
        push.origin[i] = frame.camera.origin[i];
        push.forward[i] = frame.camera.forward[i];
        push.right[i] = frame.camera.right[i];
        push.up[i] = frame.camera.up[i];
    }
    push.pad0 = 0.0f;
    push.pad1 = 0.0f;
    push.pad2 = 0.0f;
    push.pad3 = 0.0f;
    push.width = frame.width;
    push.height = frame.height;
    push.tanHalfFov = frame.camera.tanHalfFov;
    push.capacity = capacity;
    return push;
}

WavefrontLoop::Config loopConfigFor(const GradientFrame& frame, GradientRun run) {
    const bool isReplay = (run == GradientRun::Replay);

    WavefrontLoop::Config config;
    config.albedo = frame.albedo;
    config.roughness = frame.material.roughness;
    config.metallic = frame.material.metallic;
    config.specularWeight = frame.material.specularWeight;
    config.filmPixelCount = frame.filmPixelCount;

    // ONLY the replay run is given an arena. See the header for why the
    // forward run's staying 0 is a second, independent statement of "the
    // forward pass wrote no gradient".
    config.gradArenaFloats = isReplay ? frame.gradArenaFloats : 0u;
    config.gradAlbedoOffset = isReplay ? frame.gradParamOffset : 0u;
    // Likewise the adjoint seed: a property of the OBJECTIVE, not of the
    // scene, and the forward hook has no use for it.
    config.adjointSeedFloats = isReplay ? frame.adjointSeedFloats : 0u;
    // And the sensitivity map, for the same reason: a map is a derivative,
    // and the forward pass does not compute one.
    config.sensitivityFloats = isReplay ? frame.sensitivityFloats : 0u;
    // The environment image goes to BOTH, unlike the two above: it describes
    // the scene the forward run renders, not the derivative the replay run
    // takes. Same reasoning as the emission texture.
    config.envImageTexels = frame.envImageTexels;

    // Everything below goes to BOTH runs. Every one of these steers the
    // traversal or defines the film the replay run is the derivative of, and
    // steeringFieldsAgree is the assertion that they are not accidentally
    // split.
    config.diffParam = frame.diffParam;
    config.iterationSeed = frame.iterationSeed;
    config.emission = frame.emission;
    // A zero width/height leaves the traversal reading the scalar emission
    // above, exactly as a caller that never mentions a texture expects.
    config.emissionTexWidth = frame.emissionTexWidth;
    config.emissionTexHeight = frame.emissionTexHeight;
    config.emissionTexChannels = frame.emissionTexChannels;
    config.emissionUvScaleU = frame.emissionUvScaleU;
    config.emissionUvScaleV = frame.emissionUvScaleV;
    config.emissionUvBiasU = frame.emissionUvBiasU;
    config.emissionUvBiasV = frame.emissionUvBiasV;

    // THE OVERRIDE IS A SENTINEL, not a flag: Config's samplingRoughness
    // defaults to -1, which is what the shader reads as "no override". So a
    // frame with freezeSampling false must leave the whole group ALONE
    // rather than write zeros into it -- writing 0.0f would be a valid
    // roughness and would silently freeze the sampler at a mirror.
    if (frame.freezeSampling) {
        config.samplingAlbedo = frame.samplingAlbedo;
        config.samplingRoughness = frame.samplingMaterial.roughness;
        config.samplingMetallic = frame.samplingMaterial.metallic;
        config.samplingSpecularWeight = frame.samplingMaterial.specularWeight;
    }
    return config;
}

bool steeringFieldsAgree(const WavefrontLoop::Config& a, const WavefrontLoop::Config& b) {
    // EXACT float comparison, deliberately. These are not measurements to be
    // compared within a tolerance; they are the same number pushed twice, and
    // any difference at all means the two runs are not walking one path.
    return a.diffParam == b.diffParam && a.iterationSeed == b.iterationSeed &&
           a.emission == b.emission && a.emissionTexWidth == b.emissionTexWidth &&
           a.emissionTexHeight == b.emissionTexHeight &&
           a.emissionTexChannels == b.emissionTexChannels &&
           a.emissionUvScaleU == b.emissionUvScaleU && a.emissionUvScaleV == b.emissionUvScaleV &&
           a.emissionUvBiasU == b.emissionUvBiasU && a.emissionUvBiasV == b.emissionUvBiasV &&
           a.envImageTexels == b.envImageTexels &&
           a.samplingAlbedo == b.samplingAlbedo && a.samplingRoughness == b.samplingRoughness &&
           a.samplingMetallic == b.samplingMetallic &&
           a.samplingSpecularWeight == b.samplingSpecularWeight &&
           // The evaluated material steers too, whenever the sampling
           // override is absent: without it the sampler reads THESE.
           a.albedo == b.albedo && a.roughness == b.roughness && a.metallic == b.metallic &&
           a.specularWeight == b.specularWeight;
}

}  // namespace ohao::diff
