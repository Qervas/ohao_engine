// Continuous metal-rough unpack from closest-hit payload.attenuation:
//   .x = roughness [0,1]
//   .y = metallic  [0,1]
//   .z = curvature
// Legacy signed-.x encoding (negative roughness = binary metal) still accepted
// when .y is ~0 so old SBT payloads don't break mid-session.

void unpackHitPbr(vec3 att, out float roughness, out float metallic) {
    if (att.x < 0.0 && att.y < 1e-4) {
        roughness = -att.x;
        if (roughness >= 10.0) roughness -= 10.0;
        roughness = max(roughness, 0.01);
        metallic = 1.0;
    } else {
        roughness = abs(att.x);
        if (roughness >= 10.0) roughness -= 10.0;
        roughness = max(roughness, 0.01);
        metallic = clamp(att.y, 0.0, 1.0);
    }
}
