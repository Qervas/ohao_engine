#include "diff/diff_renderer.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>

namespace ohao::diff {

namespace {

/// Byte-identical to optimizer_adam.comp's Push block. Asserted, because a
/// silent mismatch here reads every field from the wrong place and still
/// produces plausible numbers.
struct AdamPush {
    std::uint32_t floatCount;
    float alpha;
    float beta1;
    float beta2;
    float epsilon;
    float oneMinusBeta1PowT;
    float oneMinusBeta2PowT;
    std::uint32_t gradOffset;
    std::uint32_t stateOffset;
};
static_assert(sizeof(AdamPush) == 36, "AdamPush must match optimizer_adam.comp's Push block");

}  // namespace

DiffRenderer::~DiffRenderer() {
    // No GpuAllocator& here, so no teardown is possible -- GradientArena's own
    // destructor has the allocator it stashed at build() and is the backstop.
    // shutdown(allocator) remains the explicit path.
}

const char* DiffRenderer::stateName(State s) noexcept {
    switch (s) {
        case State::Uninitialised: return "Uninitialised";
        case State::Configuring: return "Configuring";
        case State::Ready: return "Ready";
    }
    return "<unknown>";
}

bool DiffRenderer::init(VkDevice device, VkPhysicalDevice physicalDevice) {
    if (m_state != State::Uninitialised) return false;
    // Both, not either: a facade that half-initialises is the one that later
    // produces a plausible wrong number instead of an obvious failure.
    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE) return false;
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_state = State::Configuring;
    return true;
}

bool DiffRenderer::build(GpuAllocator& allocator) {
    if (m_state != State::Configuring) return false;
    // An empty arena is a caller error, not an empty optimisation: it would
    // build, zero, step and converge on nothing while reporting success.
    if (m_registry.count() == 0) return false;
    if (!m_arena.build(allocator, m_registry.layout())) return false;
    m_state = State::Ready;
    return true;
}

bool DiffRenderer::shutdown(GpuAllocator* allocator) {
    const bool hasArena = m_arena.buffer() != VK_NULL_HANDLE;
    if (hasArena && allocator == nullptr) {
        // Refuse rather than drop the handle on the floor. Leaving the state
        // untouched means the caller can retry with an allocator; zeroing it
        // here would strand the buffer with nothing left holding its handle.
        return false;
    }
    // Idempotent, and safe from any state: destroy() is itself idempotent, and
    // an arena that was never built has nothing to release.
    if (m_adamStageBuilt && m_device != VK_NULL_HANDLE) {
        m_adamStage.destroy(m_device);
        m_adamStageBuilt = false;
    }
    if (allocator != nullptr) m_arena.destroy(*allocator);
    // THE REGISTRY GOES WITH IT. Leaving it populated would let a second
    // lifecycle build an arena still containing the first run's blocks --
    // registered names would collide, and any that did not would be laid out
    // after stale ones. Found by the test rather than by reasoning: a
    // re-init-and-register after shutdown was rejected as a duplicate.
    m_registry = ParamRegistry{};
    m_device = VK_NULL_HANDLE;
    m_physicalDevice = VK_NULL_HANDLE;
    m_state = State::Uninitialised;
    return true;
}

RegisterResult DiffRenderer::refuseUnlessConfiguring(const char* what) const {
    if (m_state == State::Configuring) return RegisterResult{true, ParamId{}, {}};
    RegisterResult r;
    r.ok = false;
    r.error = std::string(what) + ": refused in state " + stateName(m_state) +
              ". Parameters may only be registered while Configuring -- after build() the "
              "arena layout is fixed, and a late registration would move every block that "
              "follows it, landing an in-flight kernel's gradient in a different parameter.";
    return r;
}

RegisterResult DiffRenderer::registerTexture(std::string name, ParamShape shape,
                                             VkFormat primalFormat) {
    const RegisterResult gate = refuseUnlessConfiguring("registerTexture");
    if (!gate.ok) return gate;
    return m_registry.registerTexture(std::move(name), shape, primalFormat);
}

RegisterResult DiffRenderer::registerScalarBlock(std::string name, std::uint32_t floatCount) {
    const RegisterResult gate = refuseUnlessConfiguring("registerScalarBlock");
    if (!gate.ok) return gate;
    return m_registry.registerScalarBlock(std::move(name), floatCount);
}

RegisterResult DiffRenderer::registerVertexPositions(std::string name, std::uint32_t vertexCount,
                                                     std::uint32_t componentsPerVertex) {
    const RegisterResult gate = refuseUnlessConfiguring("registerVertexPositions");
    if (!gate.ok) return gate;
    return m_registry.registerVertexPositions(std::move(name), vertexCount, componentsPerVertex);
}

bool DiffRenderer::ensureAdamStage() {
    if (m_adamStageBuilt) return true;
    if (m_device == VK_NULL_HANDLE) return false;

    // Three storage buffers: params (written in place), gradients, state.
    const std::array<VkDescriptorType, 3> bindings = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    if (!m_adamStage.build(m_device, "diff_optimizer_adam.comp.spv", bindings,
                           sizeof(AdamPush))) {
        return false;
    }
    m_adamStageBuilt = true;
    return true;
}

bool DiffRenderer::recordAdamStep(VkCommandBuffer cmd, ParamId id, VkBuffer values,
                                  const AdamSettings& settings, std::uint32_t stepIndex) {
    if (m_state != State::Ready) return false;
    if (cmd == VK_NULL_HANDLE || values == VK_NULL_HANDLE) return false;
    // 1-based, because the bias correction divides by 1 - beta^t and t = 0
    // makes that exactly zero. Rejecting beats producing an infinity.
    if (stepIndex == 0u) return false;

    const DiffParam* param = m_registry.get(id);
    if (param == nullptr || param->floatCount == 0u) return false;
    if (param->gradBlock == ArenaLayout::kInvalidBlock ||
        param->stateBlock == ArenaLayout::kInvalidBlock) {
        return false;
    }

    const ArenaBlock gradBlock = m_registry.layout().block(param->gradBlock);
    const ArenaBlock stateBlock = m_registry.layout().block(param->stateBlock);
    // sizeBytes == 0 is ArenaLayout::block's invalid marker -- offsetBytes is
    // a real location, so it must never be read without this test first.
    if (gradBlock.sizeBytes == 0u || stateBlock.sizeBytes == 0u) return false;

    if (!ensureAdamStage()) return false;

    const VkBuffer arena = m_arena.buffer();
    if (arena == VK_NULL_HANDLE) return false;
    const std::array<VkBuffer, 3> buffers = {values, arena, arena};
    if (!m_adamStage.bindBuffers(m_device, buffers)) return false;

    AdamPush push{};
    push.floatCount = param->floatCount;
    push.alpha = settings.alpha;
    push.beta1 = settings.beta1;
    push.beta2 = settings.beta2;
    push.epsilon = settings.epsilon;
    // In double, narrowed once, as the probe does: beta^t underflows slowly
    // and the host is the only place this is computed, so the two sides
    // cannot drift.
    push.oneMinusBeta1PowT = static_cast<float>(
        1.0 - std::pow(static_cast<double>(settings.beta1), static_cast<double>(stepIndex)));
    push.oneMinusBeta2PowT = static_cast<float>(
        1.0 - std::pow(static_cast<double>(settings.beta2), static_cast<double>(stepIndex)));
    // FLOAT indices, not byte offsets -- the arena's own convention.
    push.gradOffset = static_cast<std::uint32_t>(gradBlock.offsetBytes / sizeof(float));
    push.stateOffset = static_cast<std::uint32_t>(stateBlock.offsetBytes / sizeof(float));

    m_adamStage.setPushConstants(&push, sizeof(push));
    m_adamStage.setGroupCount(WavefrontStage::Fixed{(param->floatCount + 63u) / 64u});
    m_adamStage.record(cmd);
    return true;
}

bool DiffRenderer::recordGradientRun(VkCommandBuffer cmd, GradientRun run,
                                     const GradientFrame& frame,
                                     const GradientResources& resources, bool zeroArena) {
    if (m_state != State::Ready) {
        std::fprintf(stderr,
                     "[DiffRenderer] recordGradientRun refused: state is %s, not Ready. The arena "
                     "this run scatters into does not exist until build() has run\n",
                     stateName(m_state));
        return false;
    }
    return ohao::diff::recordGradientRun(cmd, run, frame, resources, m_arena, zeroArena);
}

bool DiffRenderer::zeroGradients(VkCommandBuffer cmd) {
    if (m_state != State::Ready) return false;
    if (cmd == VK_NULL_HANDLE) return false;
    m_arena.zero(cmd);
    return true;
}

}  // namespace ohao::diff
