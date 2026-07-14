#pragma once

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>

namespace ohao {

/**
 * Lightweight TRS transform (local == world until parent composition is wired).
 */
class Transform {
public:
    Transform() = default;
    explicit Transform(const glm::vec3& position,
                       const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                       const glm::vec3& scale = glm::vec3(1.0f));

    [[nodiscard]] static Transform identity() { return Transform{}; }

    void setLocalPosition(const glm::vec3& position);
    void setLocalRotation(const glm::quat& rotation);
    void setLocalScale(const glm::vec3& scale);
    void setLocalRotationEuler(const glm::vec3& eulerAngles);

    [[nodiscard]] glm::vec3 getWorldPosition() const;
    [[nodiscard]] glm::quat getWorldRotation() const;
    [[nodiscard]] glm::vec3 getWorldScale() const;

    [[nodiscard]] glm::mat4 getLocalMatrix() const;
    [[nodiscard]] glm::mat4 getWorldMatrix() const;

    [[nodiscard]] const glm::vec3& getLocalPosition() const noexcept { return localPosition; }
    [[nodiscard]] const glm::quat& getLocalRotation() const noexcept { return localRotation; }
    [[nodiscard]] const glm::vec3& getLocalScale() const noexcept { return localScale; }

    [[nodiscard]] bool isDirty() const noexcept { return dirty; }
    void setDirty() noexcept;

private:
    void updateWorldMatrix() const;

    glm::vec3 localPosition{0.0f};
    glm::quat localRotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 localScale{1.0f};

    mutable glm::mat4 localMatrix{1.0f};
    mutable glm::mat4 worldMatrix{1.0f};
    mutable bool dirty{true};
};

} // namespace ohao
