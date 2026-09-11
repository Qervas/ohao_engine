// Stage 5, check 65: the facade computes what the probe computes.
#include "probe/checks_facade_parity.hpp"

#include "diff/diff_renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ohao::diff::probe {

namespace {

constexpr std::uint32_t kFloats = 8u;
constexpr std::uint32_t kSteps = 12u;

/// A gradient with mixed signs and magnitudes spanning three decades. A
/// constant gradient would make every element's Adam trajectory identical,
/// so an offset that read the wrong element would still agree.
const std::vector<float>& gradientData() {
    static const std::vector<float> kGrad = {0.75f,   -0.25f, 0.031f, 4.5f,
                                             -1.125f, 0.008f, -2.0f,  0.4f};
    return kGrad;
}

const std::vector<float>& startValues() {
    static const std::vector<float> kStart = {1.0f,  -0.5f, 2.25f,  0.125f,
                                              -3.0f, 0.75f, -0.0625f, 5.0f};
    return kStart;
}

/// The probe's own Adam, over standalone buffers -- the path check 52 gates
/// against Kingma & Ba's algorithm listing.
bool referenceRun(ohao::diff::GpuProbeContext& ctx, std::vector<float>& values) {
    values = startValues();
    std::vector<float> state(kFloats * 2u, 0.0f);
    ohao::diff::GpuProbeContext::AdamOptions opts;  // the paper's defaults
    for (std::uint32_t t = 1; t <= kSteps; ++t) {
        if (!ctx.runAdamProbe(values, gradientData(), state, opts, t)) return false;
    }
    return true;
}

}  // namespace

bool checkFacadeParity(ohao::diff::GpuProbeContext& ctx) {
    // --- THE REFERENCE, first, so a failure to produce one is not mistaken
    // for a disagreement.
    std::vector<float> reference;
    if (!referenceRun(ctx, reference)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 65 reference Adam run\n");
        return false;
    }

    // --- THE FACADE. Same parameter, same gradient, same steps -- but the
    // gradient and the optimiser state live in the ARENA, at the offsets the
    // registry assigned, and the values buffer is updated in place.
    ohao::diff::DiffRenderer renderer;
    if (!renderer.init(ctx.device(), ctx.physicalDevice())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 65 -- DiffRenderer::init\n");
        return false;
    }
    // A FILLER PARAMETER FIRST, and it is not decoration. Registered alone,
    // the parity block lands at arena float offset 0, so `gradOffset` would be
    // zero and a kernel that ignored it entirely would still agree -- the
    // check would be exercising the one case where the thing under test
    // cannot matter. The filler pushes both blocks off zero. (Asserted below
    // rather than assumed: the layout's alignment could in principle put them
    // back.)
    const RegisterResult filler = renderer.registerScalarBlock("filler", 5u);
    if (!filler.ok) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 65 -- filler register: %s\n",
                     filler.error.c_str());
        return false;
    }
    const RegisterResult reg = renderer.registerScalarBlock("parity", kFloats);
    if (!reg.ok) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 65 -- register: %s\n",
                     reg.error.c_str());
        return false;
    }
    if (!renderer.build(ctx.allocator())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 65 -- DiffRenderer::build\n");
        return false;
    }

    const DiffParam* param = renderer.registry().get(reg.id);
    const ArenaBlock gradBlock = renderer.registry().layout().block(param->gradBlock);
    const ArenaBlock stateBlock = renderer.registry().layout().block(param->stateBlock);

    // --- NON-VACUITY OF THE OFFSETS THEMSELVES, before anything is
    // dispatched. Both must be nonzero, or this check is measuring the
    // standalone-buffer case twice under two names.
    const std::uint32_t gradFloatOffset =
        static_cast<std::uint32_t>(gradBlock.offsetBytes / sizeof(float));
    const std::uint32_t stateFloatOffset =
        static_cast<std::uint32_t>(stateBlock.offsetBytes / sizeof(float));
    if (gradFloatOffset == 0u || stateFloatOffset == 0u) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 65 -- gradient offset %u, state offset %u. "
                     "Both must be nonzero: at offset 0 a kernel that ignored the offset "
                     "entirely would agree, so the check would pass on the one case where what "
                     "it tests cannot matter\n",
                     gradFloatOffset, stateFloatOffset);
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }

    auto runFacade = [&](bool clearState, std::vector<float>& out) -> bool {
        GpuBuffer valueBuffer = ctx.allocator().createBufferFromSpan<float>(
            std::span<const float>(startValues()), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!valueBuffer.isValid()) return false;

        // Seed the arena: the gradient where the registry put it, and the
        // optimiser state zeroed -- or deliberately NOT, for the control.
        // GradientArena::build does not zero, which is hazard 1 of this stage
        // and was a real defect once (check 61).
        ctx.runImmediate([&](VkCommandBuffer cmd) {
            vkCmdUpdateBuffer(cmd, renderer.arena().buffer(), gradBlock.offsetBytes,
                              static_cast<VkDeviceSize>(gradientData().size() * sizeof(float)),
                              gradientData().data());
            if (clearState) {
                vkCmdFillBuffer(cmd, renderer.arena().buffer(), stateBlock.offsetBytes,
                                static_cast<VkDeviceSize>(stateBlock.sizeBytes), 0u);
            }
        });

        bool ok = true;
        for (std::uint32_t t = 1; t <= kSteps && ok; ++t) {
            ctx.runImmediate([&](VkCommandBuffer cmd) {
                ok = renderer.recordAdamStep(cmd, reg.id, valueBuffer.buffer,
                                             ohao::diff::DiffRenderer::AdamSettings{}, t);
            });
        }
        if (ok) {
            ctx.allocator().invalidateBuffer(valueBuffer);
            const std::span<const float> mapped =
                valueBuffer.mappedAs<const float>(static_cast<std::size_t>(kFloats));
            out.assign(mapped.begin(), mapped.end());
        }
        ctx.allocator().destroyBuffer(valueBuffer);
        return ok;
    };

    std::vector<float> facade;
    if (!runFacade(true, facade)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 65 -- facade Adam run\n");
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }

    // --- EXACT floats, not a tolerance. Both run the identical SPIR-V on the
    // identical input, so a single ulp of difference means the two are not
    // feeding it the same numbers -- which is the only thing this detects.
    if (facade.size() != reference.size()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 65 -- %zu floats against %zu\n",
                     facade.size(), reference.size());
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }
    for (std::size_t k = 0; k < facade.size(); ++k) {
        if (facade[k] != reference[k]) {
            std::fprintf(
                stderr,
                "[diff_gpu_probe] FAIL: check 65 -- element %zu: the facade gives %.9g and the "
                "probe gives %.9g, compared as EXACT floats.\n"
                "  Both dispatch the same SPIR-V on the same gradient with the same "
                "hyperparameters, so this is not precision -- it is the two paths handing the "
                "kernel different numbers. The facade reads its gradient at float offset %u and "
                "its state at %u, out of the arena; the probe binds standalone buffers and "
                "passes 0 for both. A wrong offset, or m and v taken relative to the gradient "
                "base rather than the state base, lands exactly here.\n",
                k, static_cast<double>(facade[k]), static_cast<double>(reference[k]),
                static_cast<unsigned>(gradFloatOffset),
                static_cast<unsigned>(stateFloatOffset));
            (void)renderer.shutdown(&ctx.allocator());
            return false;
        }
    }

    // --- NON-VACUITY: the values must have MOVED. Twelve steps that changed
    // nothing would agree with a reference that also changed nothing, and the
    // comparison above would pass on two columns of the starting values.
    double worstMove = 0.0;
    for (std::size_t k = 0; k < facade.size(); ++k) {
        const double moved = static_cast<double>(facade[k]) - static_cast<double>(startValues()[k]);
        if (moved < 0.0 ? -moved > worstMove : moved > worstMove) {
            worstMove = moved < 0.0 ? -moved : moved;
        }
    }
    if (!(worstMove > 1e-3)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 65 -- the values moved by at most %.9g over "
                     "%u steps. Agreeing with the reference on numbers neither of them changed "
                     "is not agreement\n",
                     worstMove, kSteps);
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }

    // --- THE CONTROL: the same run with the optimiser state left as the
    // allocator handed it back. It must NOT match, which is what proves the
    // state offsets are read at all.
    std::vector<float> dirty;
    if (!runFacade(false, dirty)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 65 control run\n");
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }
    bool controlDiffers = false;
    for (std::size_t k = 0; k < dirty.size() && !controlDiffers; ++k) {
        controlDiffers = (dirty[k] != reference[k]);
    }
    if (!controlDiffers) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 65 -- THE CONTROL MATCHED. Running without "
                     "clearing the optimiser state produced the same answer as running with it "
                     "cleared, which means the state block is not being read: m and v are "
                     "coming from somewhere else, or the arena happened to be zero. Either way "
                     "this check is not measuring the offsets it claims to\n");
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }

    if (!renderer.shutdown(&ctx.allocator())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 65 -- shutdown refused\n");
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 65 -- THE FACADE COMPUTES WHAT THE PROBE COMPUTES. "
        "%u Adam steps over a %u-float parameter, run twice: once through runAdamProbe over "
        "standalone buffers -- the path check 52 gates against Kingma & Ba's own algorithm "
        "listing -- and once through DiffRenderer::recordAdamStep, which reads the gradient at "
        "float offset %u and the optimiser state at %u OUT OF THE ARENA and updates the values "
        "in place. Compared as EXACT floats, not through a tolerance: both dispatch the same "
        "SPIR-V on the same input, so one ulp of difference would mean the two paths are "
        "handing the kernel different numbers, which is the only thing this is here to detect. "
        "The values moved %.4g at their worst element, because agreeing on numbers neither run "
        "changed is not agreement. AND THE CONTROL IS THE ARENA'S OWN HAZARD: GradientArena::"
        "build does not zero, so the same run with the optimiser state left as the allocator "
        "handed it back must NOT match -- and does not. That is what proves the state offsets "
        "are read at all, and it re-establishes for this path what check 61 established for the "
        "boundary pass. WHY THIS GATE COMES FIRST: after it, a later integration failure is the "
        "ENGINE RESOURCES rather than the loop, because the loop is pinned to the harness that "
        "64 checks already validate. BOTH OFFSETS ARE NONZERO, asserted before the dispatch "
        "and arranged by registering a filler parameter ahead of this one: at offset 0 a kernel "
        "ignoring the offset would agree, and the check would be passing on the one case where "
        "what it tests cannot matter.\n",
        kSteps, kFloats, static_cast<unsigned>(gradFloatOffset),
        static_cast<unsigned>(stateFloatOffset), worstMove);
    return true;
}

}  // namespace ohao::diff::probe
