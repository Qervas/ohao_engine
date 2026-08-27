#ifndef OHAO_DIFF_CAMERA_RAY_GLSL
#define OHAO_DIFF_CAMERA_RAY_GLSL

// Shared primary-ray direction construction. visibility_probe.comp's
// half-quad check pins this convention (ndcY > 0 maps to the top half of the
// image); wf_generate.comp must produce bit-for-bit the same rays that check
// validates, so both #include this instead of each carrying its own copy
// that could silently drift.

vec3 diffCameraRayDir(uvec2 pix, uint width, uint height, vec3 forward, vec3 right, vec3 up,
                       float tanHalfFov) {
    // Pixel centre in NDC, y down to match image row order.
    const float ndcX = (2.0 * (float(pix.x) + 0.5) / float(width) - 1.0);
    const float ndcY = (1.0 - 2.0 * (float(pix.y) + 0.5) / float(height));
    const float aspect = float(width) / float(height);

    return normalize(forward
                    + right * (ndcX * aspect * tanHalfFov)
                    + up    * (ndcY * tanHalfFov));
}

#endif  // OHAO_DIFF_CAMERA_RAY_GLSL
