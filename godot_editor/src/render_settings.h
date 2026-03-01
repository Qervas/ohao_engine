#pragma once

#include <glm/glm.hpp>

// Forward declare OHAO types
namespace ohao {
    class OffscreenRenderer;
    class DeferredRenderer;
    class PostProcessingPipeline;
}

namespace godot {

/**
 * RenderSettings - All post-processing and rendering configuration
 *
 * Extracted from OhaoViewport. Owns all render setting state and applies
 * it to the OHAO renderer's PostProcessingPipeline.
 */
class RenderSettings {
public:
    RenderSettings();

    // Apply all current settings to the renderer pipeline
    void apply(ohao::OffscreenRenderer* renderer);

    // === Render Mode ===
    void setRenderMode(int mode) { m_render_mode = mode; }
    int getRenderMode() const { return m_render_mode; }

    // === Post-Processing Toggles ===
    void setBloomEnabled(bool v)        { m_bloom_enabled = v; }
    bool getBloomEnabled() const        { return m_bloom_enabled; }
    void setTAAEnabled(bool v)          { m_taa_enabled = v; }
    bool getTAAEnabled() const          { return m_taa_enabled; }
    void setSSAOEnabled(bool v)         { m_ssao_enabled = v; }
    bool getSSAOEnabled() const         { return m_ssao_enabled; }
    void setSSGIEnabled(bool v)         { m_ssgi_enabled = v; }
    bool getSSGIEnabled() const         { return m_ssgi_enabled; }
    void setSSREnabled(bool v)          { m_ssr_enabled = v; }
    bool getSSREnabled() const          { return m_ssr_enabled; }
    void setVolumetricsEnabled(bool v)  { m_volumetrics_enabled = v; }
    bool getVolumetricsEnabled() const  { return m_volumetrics_enabled; }
    void setMotionBlurEnabled(bool v)   { m_motion_blur_enabled = v; }
    bool getMotionBlurEnabled() const   { return m_motion_blur_enabled; }
    void setDoFEnabled(bool v)          { m_dof_enabled = v; }
    bool getDoFEnabled() const          { return m_dof_enabled; }
    void setTonemappingEnabled(bool v)  { m_tonemapping_enabled = v; }
    bool getTonemappingEnabled() const  { return m_tonemapping_enabled; }

    // === Tonemapping Settings ===
    void setTonemapOperator(int op)    { m_tonemap_operator = op; }
    int getTonemapOperator() const     { return m_tonemap_operator; }
    void setExposure(float v)          { m_exposure = v; }
    float getExposure() const          { return m_exposure; }
    void setGamma(float v)             { m_gamma = v; }
    float getGamma() const             { return m_gamma; }

    // === Bloom Settings ===
    void setBloomThreshold(float v)    { m_bloom_threshold = v; }
    float getBloomThreshold() const    { return m_bloom_threshold; }
    void setBloomIntensity(float v)    { m_bloom_intensity = v; }
    float getBloomIntensity() const    { return m_bloom_intensity; }

    // === SSAO Settings ===
    void setSSAORadius(float v)        { m_ssao_radius = v; }
    float getSSAORadius() const        { return m_ssao_radius; }
    void setSSAOIntensity(float v)     { m_ssao_intensity = v; }
    float getSSAOIntensity() const     { return m_ssao_intensity; }

    // === SSGI Settings ===
    void setSSGIRadius(float v)        { m_ssgi_radius = v; }
    float getSSGIRadius() const        { return m_ssgi_radius; }
    void setSSGIIntensity(float v)     { m_ssgi_intensity = v; }
    float getSSGIIntensity() const     { return m_ssgi_intensity; }
    void setSSGISampleCount(int v)     { m_ssgi_sample_count = v; }
    int getSSGISampleCount() const     { return m_ssgi_sample_count; }

    // === SSR Settings ===
    void setSSRMaxDistance(float v)     { m_ssr_max_distance = v; }
    float getSSRMaxDistance() const     { return m_ssr_max_distance; }
    void setSSRThickness(float v)      { m_ssr_thickness = v; }
    float getSSRThickness() const      { return m_ssr_thickness; }

    // === Volumetric Settings ===
    void setVolumetricDensity(float v)     { m_volumetric_density = v; }
    float getVolumetricDensity() const     { return m_volumetric_density; }
    void setVolumetricScattering(float v)  { m_volumetric_scattering = v; }
    float getVolumetricScattering() const  { return m_volumetric_scattering; }
    void setFogColor(const glm::vec3& c)   { m_fog_color = c; }
    glm::vec3 getFogColor() const          { return m_fog_color; }

    // === Motion Blur Settings ===
    void setMotionBlurIntensity(float v)   { m_motion_blur_intensity = v; }
    float getMotionBlurIntensity() const   { return m_motion_blur_intensity; }
    void setMotionBlurSamples(int v)       { m_motion_blur_samples = v; }
    int getMotionBlurSamples() const       { return m_motion_blur_samples; }

    // === DoF Settings ===
    void setDoFFocusDistance(float v)  { m_dof_focus_distance = v; }
    float getDoFFocusDistance() const  { return m_dof_focus_distance; }
    void setDoFAperture(float v)      { m_dof_aperture = v; }
    float getDoFAperture() const      { return m_dof_aperture; }
    void setDoFMaxBlur(float v)       { m_dof_max_blur = v; }
    float getDoFMaxBlur() const       { return m_dof_max_blur; }

    // === TAA Settings ===
    void setTAABlendFactor(float v)   { m_taa_blend_factor = v; }
    float getTAABlendFactor() const   { return m_taa_blend_factor; }

    // === Sky Settings ===
    void setSkyEnabled(bool v)            { m_sky_enabled = v; }
    bool getSkyEnabled() const            { return m_sky_enabled; }
    void setSunDirection(const glm::vec3& d) { m_sun_direction = d; }
    glm::vec3 getSunDirection() const     { return m_sun_direction; }
    void setSkyTurbidity(float v)         { m_sky_turbidity = v; }
    float getSkyTurbidity() const         { return m_sky_turbidity; }
    void setSkyIntensity(float v)         { m_sky_intensity = v; }
    float getSkyIntensity() const         { return m_sky_intensity; }

    // === Rain Settings ===
    void setRainEnabled(bool v)     { m_rain_enabled   = v; }
    bool getRainEnabled() const     { return m_rain_enabled; }
    void setRainIntensity(float v)  { m_rain_intensity = v; }
    float getRainIntensity() const  { return m_rain_intensity; }
    void setRainWindX(float v)      { m_rain_wind_x    = v; }
    float getRainWindX() const      { return m_rain_wind_x; }

    // === Wetness Rate Settings ===
    void  setWetnessRate(float v)   { m_wetness_rate  = v; }
    float getWetnessRate() const    { return m_wetness_rate; }
    void  setDryingRate(float v)    { m_drying_rate   = v; }
    float getDryingRate() const     { return m_drying_rate; }

    // === Lightning Settings ===
    void  setLightningEnabled(bool v)       { m_lightning_enabled    = v; }
    bool  getLightningEnabled() const       { return m_lightning_enabled; }
    void  setLightningInterval(float v)     { m_lightning_interval   = v; }
    float getLightningInterval() const      { return m_lightning_interval; }
    void  setLightningBrightness(float v)   { m_lightning_brightness = v; }
    float getLightningBrightness() const    { return m_lightning_brightness; }

    // === Snow Settings ===
    void  setSnowEnabled(bool v)      { m_snow_enabled    = v; }
    bool  getSnowEnabled() const      { return m_snow_enabled; }
    void  setSnowIntensity(float v)   { m_snow_intensity  = v; }
    float getSnowIntensity() const    { return m_snow_intensity; }
    void  setSnowWindX(float v)       { m_snow_wind_x     = v; }
    float getSnowWindX() const        { return m_snow_wind_x; }
    void  setSnowAccumRate(float v)   { m_snow_accum_rate = v; }
    float getSnowAccumRate() const    { return m_snow_accum_rate; }
    void  setSnowMeltRate(float v)    { m_snow_melt_rate  = v; }
    float getSnowMeltRate() const     { return m_snow_melt_rate; }

    // === Cloud Settings ===
    void setCloudEnabled(bool v)          { m_cloud_enabled = v; }
    bool getCloudEnabled() const          { return m_cloud_enabled; }
    void setCloudCoverage(float v)        { m_cloud_coverage = v; }
    float getCloudCoverage() const        { return m_cloud_coverage; }
    void setCloudDensity(float v)         { m_cloud_density = v; }
    float getCloudDensity() const         { return m_cloud_density; }
    void setCloudAltMin(float v)          { m_cloud_alt_min = v; }
    float getCloudAltMin() const          { return m_cloud_alt_min; }
    void setCloudAltMax(float v)          { m_cloud_alt_max = v; }
    float getCloudAltMax() const          { return m_cloud_alt_max; }
    void setCloudSpeed(float v)           { m_cloud_speed = v; }
    float getCloudSpeed() const           { return m_cloud_speed; }

private:
    int m_render_mode = 0;  // 0=Forward, 1=Deferred

    // Post-processing toggles (all off by default; presets/scripts enable explicitly)
    bool m_bloom_enabled = false;
    bool m_taa_enabled = false;
    bool m_ssao_enabled = false;
    bool m_ssgi_enabled = false;
    bool m_ssr_enabled = false;
    bool m_volumetrics_enabled = false;
    bool m_motion_blur_enabled = false;
    bool m_dof_enabled = false;
    bool m_tonemapping_enabled = true;

    // Tonemapping
    int m_tonemap_operator = 0;
    float m_exposure = 1.0f;
    float m_gamma = 2.2f;

    // Bloom
    float m_bloom_threshold = 1.0f;
    float m_bloom_intensity = 0.5f;

    // SSAO
    float m_ssao_radius = 0.5f;
    float m_ssao_intensity = 1.0f;

    // SSGI
    float m_ssgi_radius = 3.0f;
    float m_ssgi_intensity = 1.0f;
    int m_ssgi_sample_count = 4;

    // SSR
    float m_ssr_max_distance = 100.0f;
    float m_ssr_thickness = 0.5f;

    // Volumetrics
    float m_volumetric_density = 0.02f;
    float m_volumetric_scattering = 0.8f;
    glm::vec3 m_fog_color = glm::vec3(0.7f, 0.8f, 1.0f);

    // Motion blur
    float m_motion_blur_intensity = 1.0f;
    int m_motion_blur_samples = 16;

    // DoF
    float m_dof_focus_distance = 5.0f;
    float m_dof_aperture = 2.8f;
    float m_dof_max_blur = 8.0f;

    // TAA
    float m_taa_blend_factor = 0.1f;

    // Sky
    bool      m_sky_enabled   = true;
    glm::vec3 m_sun_direction = glm::vec3(0.3f, 0.9f, 0.3f);  // normalized, toward sun
    float     m_sky_turbidity = 2.5f;
    float     m_sky_intensity = 1.0f;

    // Rain
    bool  m_rain_enabled   = false;
    float m_rain_intensity = 1.0f;
    float m_rain_wind_x    = -0.08f;

    // Wetness rates
    float m_wetness_rate   = 0.03f;
    float m_drying_rate    = 0.005f;

    // Lightning
    bool  m_lightning_enabled    = false;
    float m_lightning_interval   = 8.0f;
    float m_lightning_brightness = 3.5f;

    // Snow
    bool  m_snow_enabled    = false;
    float m_snow_intensity  = 1.0f;
    float m_snow_wind_x     = -0.08f;
    float m_snow_accum_rate = 0.02f;
    float m_snow_melt_rate  = 0.003f;

    // Clouds
    bool  m_cloud_enabled  = false;
    float m_cloud_coverage = 0.5f;
    float m_cloud_density  = 0.45f;
    float m_cloud_alt_min  = 1500.0f;
    float m_cloud_alt_max  = 8000.0f;
    float m_cloud_speed    = 1.0f;
};

} // namespace godot
