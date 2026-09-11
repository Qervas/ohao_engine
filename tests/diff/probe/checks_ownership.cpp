// Stage 5, check 66: who owns the number.
#include "probe/checks_ownership.hpp"

#include "diff/diff_renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ohao::diff::probe {

namespace {

constexpr std::uint32_t kFloats = 4u;      // one glm::vec4's worth
constexpr std::size_t kOptimised = 0u;     // the element the optimiser moves
constexpr std::size_t kEdited = 3u;        // the element a "user" later edits
constexpr float kEditValue = 9.25f;

/// A faithful stand-in for PathTracer's material ownership: a CPU-side vector
/// that is the authority, and a device buffer refreshed by copying the WHOLE
/// array over it. The copy-everything behaviour is the point -- it is what
/// makes an unsynced GPU-side write revert.
class EngineMaterial {
public:
    bool create(ohao::diff::GpuProbeContext& ctx, const std::vector<float>& initial) {
        m_cpu = initial;
        m_buffer = ctx.allocator().createBufferFromSpan<float>(
            std::span<const float>(m_cpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        return m_buffer.isValid();
    }

    /// PathTracer::setMaterialData's shape: write the CPU authority, then
    /// memcpy the whole thing to the device.
    void setMaterialData(ohao::diff::GpuProbeContext& ctx, std::size_t index, float value) {
        if (index < m_cpu.size()) m_cpu[index] = value;
        upload(ctx);
    }

    /// The re-upload on its own, for the correct protocol's sync step.
    void upload(ohao::diff::GpuProbeContext& ctx) {
        void* mapped = ctx.allocator().mapBuffer(m_buffer);
        if (mapped == nullptr) return;
        std::memcpy(mapped, m_cpu.data(), m_cpu.size() * sizeof(float));
        ctx.allocator().flushBuffer(m_buffer);
    }

    /// What the engine believes the value to be.
    [[nodiscard]] float cpuValue(std::size_t index) const {
        return index < m_cpu.size() ? m_cpu[index] : 0.0f;
    }
    void setCpuValue(std::size_t index, float v) {
        if (index < m_cpu.size()) m_cpu[index] = v;
    }

    /// What the device actually holds.
    [[nodiscard]] float deviceValue(ohao::diff::GpuProbeContext& ctx, std::size_t index) {
        ctx.allocator().invalidateBuffer(m_buffer);
        const std::span<const float> mapped =
            m_buffer.mappedAs<const float>(static_cast<std::size_t>(kFloats));
        return index < mapped.size() ? mapped[index] : 0.0f;
    }

    [[nodiscard]] VkBuffer buffer() const noexcept { return m_buffer.buffer; }
    void destroy(ohao::diff::GpuProbeContext& ctx) { ctx.allocator().destroyBuffer(m_buffer); }

private:
    std::vector<float> m_cpu;
    GpuBuffer m_buffer{};
};

const std::vector<float>& initialValues() {
    static const std::vector<float> kInit = {0.500f, 0.250f, 0.125f, 1.000f};
    return kInit;
}

}  // namespace

bool checkOwnership(ohao::diff::GpuProbeContext& ctx) {
    ohao::diff::DiffRenderer renderer;
    if (!renderer.init(ctx.device(), ctx.physicalDevice())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 66 -- init\n");
        return false;
    }
    const RegisterResult reg = renderer.registerScalarBlock("material", kFloats);
    if (!reg.ok || !renderer.build(ctx.allocator())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 66 -- register/build: %s\n",
                     reg.error.c_str());
        return false;
    }
    const DiffParam* param = renderer.registry().get(reg.id);
    const ArenaBlock gradBlock = renderer.registry().layout().block(param->gradBlock);
    const ArenaBlock stateBlock = renderer.registry().layout().block(param->stateBlock);

    // A gradient on ONE element only. The others must not move, so that the
    // revert below is unambiguous about which number was lost.
    std::vector<float> grad(kFloats, 0.0f);
    grad[kOptimised] = 1.0f;

    // `syncCpu` selects the protocol. true: read the device back into the CPU
    // authority, which is what DiffRenderer's header requires. false: leave
    // the CPU array stale, which is the design this check exists to reject.
    auto runProtocol = [&](bool syncCpu, float& afterStep, float& afterEdit) -> bool {
        EngineMaterial material;
        if (!material.create(ctx, initialValues())) return false;

        ctx.runImmediate([&](VkCommandBuffer cmd) {
            vkCmdUpdateBuffer(cmd, renderer.arena().buffer(), gradBlock.offsetBytes,
                              static_cast<VkDeviceSize>(grad.size() * sizeof(float)),
                              grad.data());
            vkCmdFillBuffer(cmd, renderer.arena().buffer(), stateBlock.offsetBytes,
                            static_cast<VkDeviceSize>(stateBlock.sizeBytes), 0u);
        });

        // THE OPTIMISER WRITES THE ENGINE'S LIVE BUFFER. That part is the same
        // in both protocols and is spec 4.3's model; what differs is only
        // whether the CPU authority is told.
        bool ok = true;
        ohao::diff::DiffRenderer::AdamSettings adam;
        adam.alpha = 0.1f;  // large, so one step is unmistakable
        ctx.runImmediate([&](VkCommandBuffer cmd) {
            ok = renderer.recordAdamStep(cmd, reg.id, material.buffer(), adam, 1u);
        });
        if (!ok) {
            material.destroy(ctx);
            return false;
        }

        afterStep = material.deviceValue(ctx, kOptimised);
        if (syncCpu) {
            // The sync the contract requires: the device is the only place the
            // new value exists, so read it into the authority before anything
            // else writes from that authority.
            material.setCpuValue(kOptimised, afterStep);
        }

        // AN ORDINARY, UNRELATED EDIT -- a user dragging a different slider.
        // PathTracer::setMaterialData rewrites the WHOLE buffer from the CPU
        // array, so this is where a stale authority destroys the optimised
        // value.
        material.setMaterialData(ctx, kEdited, kEditValue);
        afterEdit = material.deviceValue(ctx, kOptimised);

        // The unrelated element must actually have been written, or the edit
        // was a no-op and proves nothing either way.
        const float edited = material.deviceValue(ctx, kEdited);
        material.destroy(ctx);
        return edited == kEditValue;
    };

    float correctAfterStep = 0.0f, correctAfterEdit = 0.0f;
    if (!runProtocol(true, correctAfterStep, correctAfterEdit)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 66 -- correct-protocol run\n");
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }
    float staleAfterStep = 0.0f, staleAfterEdit = 0.0f;
    if (!runProtocol(false, staleAfterStep, staleAfterEdit)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 66 -- stale-protocol run\n");
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }
    (void)renderer.shutdown(&ctx.allocator());

    const float start = initialValues()[kOptimised];

    // --- NON-VACUITY: the step must have moved the value at all, and both
    // protocols must have moved it the SAME way -- they run identical GPU
    // work and differ only in what the host does afterwards.
    if (!(correctAfterStep != start) || correctAfterStep != staleAfterStep) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 66 -- the optimiser step gave %.9g (correct "
                     "protocol) and %.9g (stale), from %.9g. They must be equal and different "
                     "from the start: the two protocols perform the SAME device work and "
                     "differ only in whether the host is told\n",
                     static_cast<double>(correctAfterStep), static_cast<double>(staleAfterStep),
                     static_cast<double>(start));
        return false;
    }

    // --- THE CLAIM: syncing the authority preserves the optimised value
    // across an unrelated edit.
    if (correctAfterEdit != correctAfterStep) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 66 -- with the CPU authority synced, an "
                     "unrelated edit still changed the optimised element: %.9g became %.9g. "
                     "The sync is meant to make the engine's next whole-array upload carry the "
                     "optimised value rather than overwrite it\n",
                     static_cast<double>(correctAfterStep),
                     static_cast<double>(correctAfterEdit));
        return false;
    }

    // --- THE DEMONSTRATED FAILURE: without the sync, the same edit reverts it.
    if (staleAfterEdit != start) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 66 -- THE WRONG PROTOCOL DID NOT FAIL. With "
                     "the CPU authority left stale, an unrelated edit should have rewritten the "
                     "whole buffer from that stale array and reverted the optimised element to "
                     "%.9g; it is %.9g instead. Either the engine stand-in is not copying the "
                     "whole array the way PathTracer::setMaterialData does, in which case this "
                     "check is not modelling the hazard it claims, or the hazard is not real "
                     "here -- and the header's ownership argument would need revisiting\n",
                     static_cast<double>(start), static_cast<double>(staleAfterEdit));
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 66 -- WHO OWNS THE NUMBER. DiffRenderer's header claims the "
        "engine object owns a parameter's value while the registry owns only the gradient and "
        "the optimiser state; this is that claim as a measurement rather than a paragraph. The "
        "mechanics gated are PathTracer's own: setMaterialData writes a CPU-side vector and "
        "then memcpys THE WHOLE ARRAY to the device, so any later edit of any element rewrites "
        "every element from the CPU authority. Both protocols run the IDENTICAL device work -- "
        "the optimiser writes the engine's live buffer, which is spec 4.3's model -- and differ "
        "only in whether the host is told: element %zu moves %.6g -> %.6g in both. Then an "
        "ordinary unrelated edit of element %zu arrives. WITH the authority synced the "
        "optimised value survives at %.6g. WITHOUT it, the same edit reverts it to %.6g, which "
        "is the pre-optimisation value -- silently, with nothing logged and nothing failing. "
        "That is the demonstrated failure, and it is why the obvious design (let the registry "
        "hold the value and write the device buffer) is wrong: two authorities for one number "
        "resolve to whichever wrote last. STILL OWED: this gates the protocol against a "
        "faithful reproduction of setMaterialData's map-and-memcpy, not against PathTracer "
        "itself, which needs its images and pipelines to stand up.\n",
        kOptimised, static_cast<double>(start), static_cast<double>(correctAfterStep), kEdited,
        static_cast<double>(correctAfterEdit), static_cast<double>(staleAfterEdit));
    return true;
}

}  // namespace ohao::diff::probe
