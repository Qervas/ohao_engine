#include "diff/diff_renderer.hpp"

#include <string>

namespace ohao::diff {

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

bool DiffRenderer::zeroGradients(VkCommandBuffer cmd) {
    if (m_state != State::Ready) return false;
    if (cmd == VK_NULL_HANDLE) return false;
    m_arena.zero(cmd);
    return true;
}

}  // namespace ohao::diff
