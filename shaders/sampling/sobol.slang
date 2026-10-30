// Owen-scrambled Sobol (Burley 2020, "Practical Hash-based Owen Scrambling", JCGT 9(4)): the path
// tracer's random numbers in both profiles (frontier F1 T2). A pixel's samples are one sequence,
// stratified across its frames however many of them there are; pixels are decorrelated by a seed.
//
// Draws come in groups of four: a group is one point of the 4-D Sobol sequence, its index shuffled
// and its coordinates scrambled by a seed hashed from the pixel's and the group's number (Burley's
// padding), so dimensions of different groups are independent and the dimensions within a group
// are jointly stratified. A caller places related draws in one group (Sampler.group).
//
// ohao/render/rt/sobol_generator.cpp (the direction numbers, Joe-Kuo new-joe-kuo-6.21201) and
// owen_scramble.cpp (the scramble) are its CPU twin; tests/renderer/sobol_test.cpp runs this module
// through slangc's C++ (tools/sampling) against them.

// Joe-Kuo's direction numbers for dimensions 0-3, 32 a dimension (dimension 0 is the bit reversal).
static const uint kSobolDirections[128] = {
    0x80000000u, 0x40000000u, 0x20000000u, 0x10000000u, 0x08000000u, 0x04000000u, 0x02000000u, 0x01000000u,
    0x00800000u, 0x00400000u, 0x00200000u, 0x00100000u, 0x00080000u, 0x00040000u, 0x00020000u, 0x00010000u,
    0x00008000u, 0x00004000u, 0x00002000u, 0x00001000u, 0x00000800u, 0x00000400u, 0x00000200u, 0x00000100u,
    0x00000080u, 0x00000040u, 0x00000020u, 0x00000010u, 0x00000008u, 0x00000004u, 0x00000002u, 0x00000001u,
    0x80000000u, 0xC0000000u, 0xA0000000u, 0xF0000000u, 0x88000000u, 0xCC000000u, 0xAA000000u, 0xFF000000u,
    0x80800000u, 0xC0C00000u, 0xA0A00000u, 0xF0F00000u, 0x88880000u, 0xCCCC0000u, 0xAAAA0000u, 0xFFFF0000u,
    0x80008000u, 0xC000C000u, 0xA000A000u, 0xF000F000u, 0x88008800u, 0xCC00CC00u, 0xAA00AA00u, 0xFF00FF00u,
    0x80808080u, 0xC0C0C0C0u, 0xA0A0A0A0u, 0xF0F0F0F0u, 0x88888888u, 0xCCCCCCCCu, 0xAAAAAAAAu, 0xFFFFFFFFu,
    0x80000000u, 0xC0000000u, 0x60000000u, 0x90000000u, 0xE8000000u, 0x5C000000u, 0x8E000000u, 0xC5000000u,
    0x68800000u, 0x9CC00000u, 0xEE600000u, 0x55900000u, 0x80680000u, 0xC09C0000u, 0x60EE0000u, 0x90550000u,
    0xE8808000u, 0x5CC0C000u, 0x8E606000u, 0xC5909000u, 0x6868E800u, 0x9C9C5C00u, 0xEEEE8E00u, 0x5555C500u,
    0x8000E880u, 0xC0005CC0u, 0x60008E60u, 0x9000C590u, 0xE8006868u, 0x5C009C9Cu, 0x8E00EEEEu, 0xC5005555u,
    0x80000000u, 0xC0000000u, 0x20000000u, 0x50000000u, 0xF8000000u, 0x74000000u, 0xA2000000u, 0x93000000u,
    0xD8800000u, 0x25400000u, 0x59E00000u, 0xE6D00000u, 0x78080000u, 0xB40C0000u, 0x82020000u, 0xC3050000u,
    0x208F8000u, 0x51474000u, 0xFBEA2000u, 0x75D93000u, 0xA0858800u, 0x914E5400u, 0xDBE79E00u, 0x25DB6D00u,
    0x58800080u, 0xE54000C0u, 0x79E00020u, 0xB6D00050u, 0x800800F8u, 0xC00C0074u, 0x200200A2u, 0x50050093u
};

uint sobolBits(uint index, uint dim)
{
    uint x = 0u;
    for (uint bit = 0u; index != 0u; ++bit, index >>= 1u)
    {
        if ((index & 1u) != 0u) x ^= kSobolDirections[dim * 32u + bit];
    }
    return x;
}

// The bits of v in reverse order (written out: slangc's C++ target has no reversebits).
uint reverseBits(uint v)
{
    v = (v << 16u) | (v >> 16u);
    v = ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
    v = ((v & 0x0F0F0F0Fu) << 4u) | ((v & 0xF0F0F0F0u) >> 4u);
    v = ((v & 0x33333333u) << 2u) | ((v & 0xCCCCCCCCu) >> 2u);
    return ((v & 0x55555555u) << 1u) | ((v & 0xAAAAAAAAu) >> 1u);
}

// Burley's nested uniform scramble: a hash that only ever looks at higher bits, applied to the
// bit-reversed value (Vegdahl's permutation constants).
uint owenScramble(uint v, uint seed)
{
    v = reverseBits(v);
    v ^= v * 0x3d20adeau;
    v += seed;
    v *= (seed >> 16u) | 1u;
    v ^= v * 0x05526c56u;
    v ^= v * 0x53a22864u;
    return reverseBits(v);
}

uint hashCombine(uint seed, uint v) { return seed ^ (v + (seed << 6u) + (seed >> 2u)); }

// The 4-D point `index` of the sequence scrambled by `seed`, each coordinate in [0, 1).
float4 sobol4(uint index, uint seed)
{
    const uint i = owenScramble(index, seed);
    float4 p;
    [unroll] for (uint d = 0u; d < 4u; ++d)
        p[d] = float(owenScramble(sobolBits(i, d), hashCombine(seed, d)) >> 8u) * (1.0 / 16777216.0);
    return p;
}

// A pixel's stream of draws for one sample: `index` the sample's number, `seed` the pixel's.
struct Sampler
{
    uint index;
    uint seed;
    uint dim;        // the next draw's dimension
    float4 point;    // the current group's point

    __init(uint sampleIndex, uint pixelSeed)
    {
        index = sampleIndex;
        seed = pixelSeed;
        dim = 0u;
        point = float4(0.0);
    }

    // Moves to the start of group `g`: draws after this are its coordinates.
    [mutating] void group(uint g) { dim = g * 4u; }

    [mutating] float next()
    {
        if ((dim & 3u) == 0u) point = sobol4(index, hashCombine(seed, dim >> 2u));
        const float u = point[dim & 3u];
        ++dim;
        return u;
    }
};
