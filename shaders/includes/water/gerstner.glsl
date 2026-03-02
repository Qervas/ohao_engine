#ifndef OHAO_GERSTNER_GLSL
#define OHAO_GERSTNER_GLSL

// Extracted Gerstner wave functions shared by water.vert and any future variant.
// Include via:  #include "water/gerstner.glsl"
// (CMake compiles with -I<shaders/includes> so path is relative to that root)

const float PI = 3.14159265359;

struct GerstnerWave {
    vec2  dir;
    float wavelength;
    float steepness;
    float speed;
};

// Returns the XZ horizontal displacement + Y vertical displacement for one wave.
// Gerstner formula:
//   X += Q * A * D.x * cos(k * dot(D, P.xz) - omega * t)
//   Z += Q * A * D.y * cos(k * dot(D, P.xz) - omega * t)
//   Y +=     A       * sin(k * dot(D, P.xz) - omega * t)
// where k = 2π/λ, omega = sqrt(9.81 * k) * speed, Q = steepness / (k * A)
vec3 gerstnerDisplace(vec3 pos, GerstnerWave w, float time) {
    float k = 2.0 * PI / w.wavelength;
    float omega = sqrt(9.81 / k) * w.speed;
    vec2  d = normalize(w.dir);
    float f = k * dot(d, pos.xz) - omega * time;
    float A = w.steepness / k;
    float Q = w.steepness / (k * max(A, 0.0001));
    return vec3(
        Q * A * d.x * cos(f),
        A * sin(f),
        Q * A * d.y * cos(f)
    );
}

// Returns the normal perturbation contribution for one wave.
// N += (-D.x * k * A * cos(f), -Q * k * A * sin(f), -D.y * k * A * cos(f))
vec3 gerstnerNormal(vec3 pos, GerstnerWave w, float time) {
    float k = 2.0 * PI / w.wavelength;
    float omega = sqrt(9.81 / k) * w.speed;
    vec2  d = normalize(w.dir);
    float f = k * dot(d, pos.xz) - omega * time;
    float A = w.steepness / k;
    float Q = w.steepness / (k * max(A, 0.0001));
    return vec3(
        -d.x * k * A * cos(f),
        -Q   * k * A * sin(f),
        -d.y * k * A * cos(f)
    );
}

#endif // OHAO_GERSTNER_GLSL
