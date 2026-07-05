#ifndef OHAO_SAMPLER_PCG_GLSL
#define OHAO_SAMPLER_PCG_GLSL

// PCG-based 1D/2D sampler. Legacy implementation; used by realtime
// profile and as a fallback sampler choice for offline.

uint _pcg_state;

void samplerInit_pcg(uvec2 pixel, uint sampleIdx) {
    _pcg_state = pixel.x * 1973u + pixel.y * 9277u + sampleIdx * 26699u + 1u;
}

uint _pcg_next() {
    _pcg_state = _pcg_state * 747796405u + 2891336453u;
    uint w = ((_pcg_state >> ((_pcg_state >> 28u) + 4u)) ^ _pcg_state) * 277803737u;
    return (w >> 22u) ^ w;
}

// dim unused by stateful PCG; reserved for future stratification
float getSample1D_pcg(uint dim) {
    return float(_pcg_next()) / 4294967296.0;
}

// dim unused by stateful PCG; reserved for future stratification
vec2 getSample2D_pcg(uint dim) {
    float x = float(_pcg_next()) / 4294967296.0;
    float y = float(_pcg_next()) / 4294967296.0;
    return vec2(x, y);
}

#endif
