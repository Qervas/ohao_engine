#ifndef OHAO_DIFF_PATH_STATE_GLSL
#define OHAO_DIFF_PATH_STATE_GLSL

// GLSL mirror of ohao/diff/wavefront/path_state_layout.hpp. The two are a
// matched pair like PathRng and rng.glsl: change both or neither. The probe's
// field round-trip check (diff_gpu_probe.cpp) is what proves they agree.
//
// Binding scheme, fixed here for every wavefront stage: this buffer -- the
// SoA path-state arena -- is always bound at binding 0. Queues are a `uint`
// buffer at binding 1; counters at binding 2 (declared by each stage, not
// here, since which queue/slot a stage touches varies per stage).
//
// Field offsets are NOT passed as push-constant data. ArenaLayout aligns
// every block to 256 bytes and every field is exactly `capacity` floats, so
// the offset of field `i` (in floats) is a pure function of `capacity`:
//
//     offsetFloats(i) = i * alignUp(capacity, 64)
//
// This was proven algebraically during Task 1's review: ArenaLayout::add
// gives offsetBytes(i) = i * alignUp(capacity*4, 256) by induction (every
// block in this arena is the same size), and because 256 == 4*64 the
// identity alignUp(4c, 256) == 4*alignUp(c, 64) holds exactly. Every stage
// therefore only needs to push `capacity` (one uint), not 16 offsets -- a
// push-constant array of 16 uints (64 bytes) plus a camera block (80 bytes,
// see camera_ray.glsl / visibility_probe.comp) would exceed the 128-byte
// push-constant size Vulkan guarantees on every implementation.

layout(std430, binding = 0) buffer PathStateBuffer {
    float data[];
} psState;

// -- Field indices, in ohao::diff::PathStateField enum order. Do not
// reorder without reordering the C++ enum identically. --
const uint PS_ORIGIN_X      = 0u;
const uint PS_ORIGIN_Y      = 1u;
const uint PS_ORIGIN_Z      = 2u;
const uint PS_DIR_X         = 3u;
const uint PS_DIR_Y         = 4u;
const uint PS_DIR_Z         = 5u;
const uint PS_THROUGHPUT_R  = 6u;
const uint PS_THROUGHPUT_G  = 7u;
const uint PS_THROUGHPUT_B  = 8u;
const uint PS_RADIANCE_R    = 9u;
const uint PS_RADIANCE_G    = 10u;
const uint PS_RADIANCE_B    = 11u;
const uint PS_PIXEL_INDEX   = 12u;  // bit-cast uint
const uint PS_SAMPLE_INDEX  = 13u;  // bit-cast uint
const uint PS_BOUNCE        = 14u;  // bit-cast uint
const uint PS_ALIVE         = 15u;  // bit-cast uint, 0 or 1
const uint PS_HIT_T         = 16u;  // intersect-stage output: hit distance, -1 on miss (Task 5)
const uint PS_FIELD_COUNT   = 17u;

// Every ArenaLayout block is aligned to 256 bytes == 64 floats.
const uint PS_ALIGNMENT_FLOATS = 64u;

uint psAlignUp(uint value, uint alignment) {
    return ((value + alignment - 1u) / alignment) * alignment;
}

// Stride (in floats) between one field's block and the next, for an arena
// built with this `capacity`.
uint psFieldStride(uint capacity) {
    return psAlignUp(capacity, PS_ALIGNMENT_FLOATS);
}

// Float offset of field `fieldIndex`'s block, for an arena built with this
// `capacity`.
uint psFieldOffset(uint fieldIndex, uint capacity) {
    return fieldIndex * psFieldStride(capacity);
}

float psGetScalar(uint fieldIndex, uint pathIndex, uint capacity) {
    return psState.data[psFieldOffset(fieldIndex, capacity) + pathIndex];
}

void psSetScalar(uint fieldIndex, uint pathIndex, uint capacity, float value) {
    psState.data[psFieldOffset(fieldIndex, capacity) + pathIndex] = value;
}

// -- vec3 accessors --

vec3 psGetOrigin(uint pathIndex, uint capacity) {
    return vec3(psGetScalar(PS_ORIGIN_X, pathIndex, capacity),
                psGetScalar(PS_ORIGIN_Y, pathIndex, capacity),
                psGetScalar(PS_ORIGIN_Z, pathIndex, capacity));
}

void psSetOrigin(uint pathIndex, uint capacity, vec3 v) {
    psSetScalar(PS_ORIGIN_X, pathIndex, capacity, v.x);
    psSetScalar(PS_ORIGIN_Y, pathIndex, capacity, v.y);
    psSetScalar(PS_ORIGIN_Z, pathIndex, capacity, v.z);
}

vec3 psGetDir(uint pathIndex, uint capacity) {
    return vec3(psGetScalar(PS_DIR_X, pathIndex, capacity),
                psGetScalar(PS_DIR_Y, pathIndex, capacity),
                psGetScalar(PS_DIR_Z, pathIndex, capacity));
}

void psSetDir(uint pathIndex, uint capacity, vec3 v) {
    psSetScalar(PS_DIR_X, pathIndex, capacity, v.x);
    psSetScalar(PS_DIR_Y, pathIndex, capacity, v.y);
    psSetScalar(PS_DIR_Z, pathIndex, capacity, v.z);
}

vec3 psGetThroughput(uint pathIndex, uint capacity) {
    return vec3(psGetScalar(PS_THROUGHPUT_R, pathIndex, capacity),
                psGetScalar(PS_THROUGHPUT_G, pathIndex, capacity),
                psGetScalar(PS_THROUGHPUT_B, pathIndex, capacity));
}

void psSetThroughput(uint pathIndex, uint capacity, vec3 v) {
    psSetScalar(PS_THROUGHPUT_R, pathIndex, capacity, v.x);
    psSetScalar(PS_THROUGHPUT_G, pathIndex, capacity, v.y);
    psSetScalar(PS_THROUGHPUT_B, pathIndex, capacity, v.z);
}

vec3 psGetRadiance(uint pathIndex, uint capacity) {
    return vec3(psGetScalar(PS_RADIANCE_R, pathIndex, capacity),
                psGetScalar(PS_RADIANCE_G, pathIndex, capacity),
                psGetScalar(PS_RADIANCE_B, pathIndex, capacity));
}

void psSetRadiance(uint pathIndex, uint capacity, vec3 v) {
    psSetScalar(PS_RADIANCE_R, pathIndex, capacity, v.x);
    psSetScalar(PS_RADIANCE_G, pathIndex, capacity, v.y);
    psSetScalar(PS_RADIANCE_B, pathIndex, capacity, v.z);
}

// -- Integer accessors (bit-cast through the same float storage) --

uint psGetPixelIndex(uint pathIndex, uint capacity) {
    return floatBitsToUint(psGetScalar(PS_PIXEL_INDEX, pathIndex, capacity));
}

void psSetPixelIndex(uint pathIndex, uint capacity, uint value) {
    psSetScalar(PS_PIXEL_INDEX, pathIndex, capacity, uintBitsToFloat(value));
}

uint psGetSampleIndex(uint pathIndex, uint capacity) {
    return floatBitsToUint(psGetScalar(PS_SAMPLE_INDEX, pathIndex, capacity));
}

void psSetSampleIndex(uint pathIndex, uint capacity, uint value) {
    psSetScalar(PS_SAMPLE_INDEX, pathIndex, capacity, uintBitsToFloat(value));
}

uint psGetBounce(uint pathIndex, uint capacity) {
    return floatBitsToUint(psGetScalar(PS_BOUNCE, pathIndex, capacity));
}

void psSetBounce(uint pathIndex, uint capacity, uint value) {
    psSetScalar(PS_BOUNCE, pathIndex, capacity, uintBitsToFloat(value));
}

uint psGetAlive(uint pathIndex, uint capacity) {
    return floatBitsToUint(psGetScalar(PS_ALIVE, pathIndex, capacity));
}

void psSetAlive(uint pathIndex, uint capacity, uint value) {
    psSetScalar(PS_ALIVE, pathIndex, capacity, uintBitsToFloat(value));
}

float psGetHitT(uint pathIndex, uint capacity) {
    return psGetScalar(PS_HIT_T, pathIndex, capacity);
}

void psSetHitT(uint pathIndex, uint capacity, float value) {
    psSetScalar(PS_HIT_T, pathIndex, capacity, value);
}

#endif  // OHAO_DIFF_PATH_STATE_GLSL
