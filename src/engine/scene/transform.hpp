#pragma once
#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>

namespace ohao {

class Transform {
public:
    Transform() = default;
    Transform(const glm::vec3& position, const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
             const glm::vec3& scale = glm::vec3(1.0f));

    void setLocalPosition(const glm::vec3& position);
    void setLocalRotation(const glm::quat& rotation);
    void setLocalScale(const glm::vec3& scale);

    void setLocalRotationEuler(const glm::vec3& eulerAngles);

    glm::vec3 getWorldPosition() const;
    glm::quat getWorldRotation() const;
    glm::vec3 getWorldScale() const;

    glm::mat4 getLocalMatrix() const;
    glm::mat4 getWorldMatrix() const;

    const glm::vec3& getLocalPosition() const { return localPosition; }
    const glm::quat& getLocalRotation() const { return localRotation; }
    const glm::vec3& getLocalScale() const { return localScale; }

    void setDirty();

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
