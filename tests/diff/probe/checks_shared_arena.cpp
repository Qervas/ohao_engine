// Stage 3, check 61: the interior and boundary terms in ONE arena.
#include "probe/checks_shared_arena.hpp"

#include "diff/grad/arena_layout.hpp"
#include "diff/grad/gradient_arena.hpp"
#include "diff/param/param_registry.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ohao::diff::probe {

bool checkSharedArena(ohao::diff::GpuProbeContext& ctx) {
    constexpr std::uint32_t kImage = 8u;
    constexpr float kLIn = 3.0f, kLOut = 0.5f;
    constexpr std::uint32_t kVertices = 3u;
    constexpr std::uint32_t kComponents = 2u;

    // TWO PARAMETERS OF DIFFERENT KINDS in one registry. The albedo is an
    // appearance parameter, so spec 4.1 says its boundary term is exactly
    // zero; the triangle is geometry, the only kind for which that term is
    // nonzero. Registering both is what makes "summed into the same arena" a
    // statement with content rather than a description of one buffer.
    ohao::diff::ParamRegistry reg;
    const auto regAlbedo = reg.registerScalarBlock("albedo", 1);
    const auto regVerts = reg.registerVertexPositions("triangle", kVertices, kComponents);
    if (!regAlbedo.ok || !regVerts.ok) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 61 registry setup: %s %s\n",
                     regAlbedo.error.c_str(), regVerts.error.c_str());
        return false;
    }
    const ohao::diff::DiffParam* albedo = reg.find("albedo");
    const ohao::diff::DiffParam* verts = reg.find("triangle");
    if (albedo == nullptr || verts == nullptr) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 61 params not found\n");
        return false;
    }
    if (verts->kind != ohao::diff::ParamKind::VertexPositions) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 61 -- the triangle registered as kind %d, not "
                     "VertexPositions. The kind is what says a boundary term exists for this "
                     "block, so a geometry parameter recorded as a ScalarBlock erases the fact "
                     "spec 4.1's split rests on\n",
                     static_cast<int>(verts->kind));
        return false;
    }
    if (verts->floatCount != kVertices * kComponents) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 61 -- the block holds %u floats, "
                              "expected %u\n",
                     verts->floatCount, kVertices * kComponents);
        return false;
    }

    ohao::diff::GradientArena arena;
    if (!arena.build(ctx.allocator(), reg.layout())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 61 arena build\n");
        return false;
    }
    const auto arenaFloats =
        static_cast<std::uint32_t>(reg.layout().totalBytes() / sizeof(float));
    const auto vertOffset = static_cast<std::uint32_t>(
        reg.layout().block(verts->gradBlock).offsetBytes / sizeof(float));
    const auto albedoOffset = static_cast<std::uint32_t>(
        reg.layout().block(albedo->gradBlock).offsetBytes / sizeof(float));

    const std::vector<float> triangle = {1.7f, 1.3f, 2.1f, 6.4f, 6.8f, 3.9f};
    const std::vector<std::uint32_t> edges = {0u, 1u, 1u, 2u, 2u, 0u};

    // --- STANDALONE, for the value to compare against.
    std::vector<float> standalone;
    if (!ctx.runBoundaryProbe(triangle, edges, kImage, kImage, {kLIn, kLOut}, {}, {}, nullptr, 0u,
                              0u, standalone)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 61 standalone dispatch\n");
        arena.destroy(ctx.allocator());
        return false;
    }

    // --- INTO THE ARENA, at the registered block's own float offset.
    std::vector<float> inArena;
    if (!ctx.runBoundaryProbe(triangle, edges, kImage, kImage, {kLIn, kLOut}, {}, {}, &arena,
                              verts->gradBlock, vertOffset, inArena)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 61 arena dispatch\n");
        arena.destroy(ctx.allocator());
        return false;
    }

    const std::vector<float> whole = arena.readbackAll(ctx.allocator());
    arena.destroy(ctx.allocator());

    if (inArena.size() != standalone.size()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 61 -- %zu floats from the arena, %zu "
                              "standalone\n",
                     inArena.size(), standalone.size());
        return false;
    }
    // --- THE SAME NUMBERS, wherever they were written. The offset must not
    // change the value, only its address.
    double scale = 0.0;
    for (float g : standalone) scale = std::max(scale, std::fabs(static_cast<double>(g)));
    if (!(scale > 0.1)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 61 -- the standalone gradient is %.9g at its "
                     "largest, so agreeing with the arena run would mean nothing\n",
                     scale);
        return false;
    }
    for (std::size_t k = 0; k < inArena.size(); ++k) {
        if (std::fabs(static_cast<double>(inArena[k] - standalone[k])) > 1e-5 * scale) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 61 -- component %zu is %.9g in the arena "
                         "and %.9g standalone. The offset changes WHERE the gradient is written, "
                         "never what it is\n",
                         k, static_cast<double>(inArena[k]),
                         static_cast<double>(standalone[k]));
            return false;
        }
    }

    // --- AND NOTHING ELSE IN THE ARENA MOVED. This is the assertion that
    // makes a shared arena safe: the albedo's gradient block, both
    // parameters' Adam state, and the 256-byte alignment padding are all
    // EXACTLY 0.0f -- compared as floats, not through a tolerance. A boundary
    // kernel writing at the wrong offset lands in an appearance parameter's
    // block, where it would look like a perfectly ordinary gradient.
    if (whole.size() != arenaFloats) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 61 -- whole-arena readback gave %zu "
                              "floats, expected %u\n",
                     whole.size(), arenaFloats);
        return false;
    }
    std::size_t stray = 0;
    std::size_t firstStray = 0;
    for (std::uint32_t k = 0; k < arenaFloats; ++k) {
        const bool inVertexBlock = k >= vertOffset && k < vertOffset + verts->floatCount;
        if (inVertexBlock) continue;
        if (whole[k] != 0.0f) {
            if (stray == 0) firstStray = k;
            ++stray;
        }
    }
    if (stray != 0) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 61 -- %zu of the %u arena floats outside the "
                     "triangle's gradient block are nonzero; the first is float %zu at %.9g. The "
                     "albedo's block starts at float %u, so a boundary kernel writing at the "
                     "wrong offset lands in an APPEARANCE parameter -- where spec 4.1 says the "
                     "boundary term is exactly zero, and where the damage would look like an "
                     "ordinary gradient\n",
                     stray, arenaFloats, firstStray, static_cast<double>(whole[firstStray]),
                     albedoOffset);
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 61 -- THE INTERIOR AND BOUNDARY TERMS SHARE ONE ARENA, which "
        "spec 4.1 asks for and which nothing before this check made true: the boundary pass wrote "
        "its own buffer, so the two could not have interfered and the regression gate over checks "
        "37-55 held for free. Two parameters of DIFFERENT KINDS are registered together -- an "
        "albedo ScalarBlock, whose boundary term is mathematically absent, and a "
        "VertexPositions block of %u vertices, the only kind for which it is not. The kind is "
        "asserted, not assumed: registering geometry as a ScalarBlock would erase the fact the "
        "whole split rests on. The boundary pass writes at the registered block's own FLOAT "
        "offset (%u), the same convention the interior kernel uses for its parameter, and the "
        "values are identical to a standalone run -- the offset changes where a gradient is "
        "written, never what it is. THEN THE NULL TEST: all %u other arena floats are EXACTLY "
        "0.0f, compared as floats. That covers the albedo's block at float %u, both parameters' "
        "Adam state, and the alignment padding -- and it is the assertion that makes sharing "
        "safe, because a kernel writing at the wrong offset lands in an appearance parameter "
        "where the damage would look like an ordinary gradient.\n",
        kVertices, vertOffset, arenaFloats - verts->floatCount, albedoOffset);
    return true;
}

}  // namespace ohao::diff::probe
