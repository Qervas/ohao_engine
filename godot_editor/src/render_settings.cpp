#include "render_settings.h"

#include "renderer/offscreen/offscreen_renderer.hpp"
#include "renderer/passes/deferred_renderer.hpp"
#include "renderer/passes/post_processing_pipeline.hpp"

namespace godot {

RenderSettings::RenderSettings() {}

void RenderSettings::apply(ohao::OffscreenRenderer* renderer) {
    if (!renderer) return;

    ohao::DeferredRenderer* deferred = renderer->getDeferredRenderer();
    if (!deferred) return;

    ohao::PostProcessingPipeline* pp = deferred->getPostProcessing();
    if (!pp) return;

    // Post-processing toggles
    pp->setBloomEnabled(m_bloom_enabled);
    pp->setTAAEnabled(m_taa_enabled);
    pp->setSSAOEnabled(m_ssao_enabled);
    pp->setSSGIEnabled(m_ssgi_enabled);
    pp->setSSREnabled(m_ssr_enabled);
    pp->setVolumetricsEnabled(m_volumetrics_enabled);
    pp->setMotionBlurEnabled(m_motion_blur_enabled);
    pp->setDoFEnabled(m_dof_enabled);
    pp->setTonemappingEnabled(m_tonemapping_enabled);

    // Tonemapping
    pp->setTonemapOperator(static_cast<ohao::TonemapOperator>(m_tonemap_operator));
    pp->setExposure(m_exposure);
    pp->setGamma(m_gamma);

    // Bloom
    pp->setBloomThreshold(m_bloom_threshold);
    pp->setBloomIntensity(m_bloom_intensity);

    // SSAO
    pp->setSSAORadius(m_ssao_radius);
    pp->setSSAOIntensity(m_ssao_intensity);

    // SSGI
    pp->setSSGIRadius(m_ssgi_radius);
    pp->setSSGIIntensity(m_ssgi_intensity);
    pp->setSSGISampleCount(static_cast<uint32_t>(m_ssgi_sample_count));

    // SSR
    pp->setSSRMaxDistance(m_ssr_max_distance);
    pp->setSSRThickness(m_ssr_thickness);

    // Volumetrics
    pp->setVolumetricDensity(m_volumetric_density);
    pp->setVolumetricScattering(m_volumetric_scattering);
    pp->setFogColor(m_fog_color);

    // Motion blur
    pp->setMotionBlurIntensity(m_motion_blur_intensity);
    pp->setMotionBlurSamples(static_cast<uint32_t>(m_motion_blur_samples));

    // DoF
    pp->setDoFFocusDistance(m_dof_focus_distance);
    pp->setDoFAperture(m_dof_aperture);
    pp->setDoFMaxBlurRadius(m_dof_max_blur);

    // TAA
    pp->setTAABlendFactor(m_taa_blend_factor);

    // Sky (applied to DeferredRenderer directly, not PostProcessingPipeline)
    deferred->setSkyEnabled(m_sky_enabled);
    deferred->setSkyTurbidity(m_sky_turbidity);
    deferred->setSkyIntensity(m_sky_intensity);
    // Only update sun direction if sky is enabled (avoids overwriting CSM light direction)
    if (m_sky_enabled) {
        deferred->setSunDirection(m_sun_direction);
    }

    // Clouds
    deferred->setCloudEnabled(m_cloud_enabled);
    deferred->setCloudCoverage(m_cloud_coverage);
    deferred->setCloudDensity(m_cloud_density);
    deferred->setCloudAltMin(m_cloud_alt_min);
    deferred->setCloudAltMax(m_cloud_alt_max);
    deferred->setCloudSpeed(m_cloud_speed);
}

} // namespace godot
