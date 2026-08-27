#include "diff/param/param_registry.hpp"

namespace ohao::diff {

bool ParamRegistry::isDifferentiableFormat(VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R32G32B32_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16_SFLOAT:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16_SFLOAT:
        return true;
    default:
        return false;
    }
}

RegisterResult ParamRegistry::addParam(std::string name, ParamKind kind, ParamShape shape,
                                       std::uint32_t floatCount) {
    RegisterResult result;

    if (name.empty()) {
        result.error = "parameter name must not be empty";
        return result;
    }
    if (floatCount == 0) {
        result.error = "parameter '" + name + "' has zero floats";
        return result;
    }
    if (find(name) != nullptr) {
        result.error = "parameter '" + name + "' is already registered";
        return result;
    }

    DiffParam p;
    p.name = std::move(name);
    p.kind = kind;
    p.shape = shape;
    p.floatCount = floatCount;
    p.gradBlock = m_layout.add(floatCount);
    p.stateBlock = m_layout.add(floatCount * 2u);  // Adam m and v

    m_params.push_back(std::move(p));

    result.ok = true;
    result.id.value = static_cast<std::uint32_t>(m_params.size() - 1);
    return result;
}

RegisterResult ParamRegistry::registerTexture(std::string name, ParamShape shape,
                                              VkFormat primalFormat) {
    if (!isDifferentiableFormat(primalFormat)) {
        RegisterResult result;
        result.error =
            "parameter '" + name +
            "': primal texture is not a float format. 8-bit storage cannot be optimized "
            "(an Adam step below 1/255 rounds to nothing and the fit stalls silently). "
            "Call promoteToFloat(handle) on the bindless texture first.";
        return result;
    }
    return addParam(std::move(name), ParamKind::Texture, shape, shape.floatCount());
}

RegisterResult ParamRegistry::registerScalarBlock(std::string name, std::uint32_t floatCount) {
    return addParam(std::move(name), ParamKind::ScalarBlock, ParamShape{}, floatCount);
}

const DiffParam* ParamRegistry::find(std::string_view name) const {
    for (const auto& p : m_params) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

const DiffParam* ParamRegistry::get(ParamId id) const {
    if (!id.valid() || id.value >= m_params.size()) return nullptr;
    return &m_params[id.value];
}

}  // namespace ohao::diff
