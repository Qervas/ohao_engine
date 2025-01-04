#pragma once
#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>

namespace ohao {

class SceneNode;

class Transform {
public:
    Transform() = default;
    Transform(const glm::vec3& position, const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
             const glm::vec3& scale = glm::vec3(1.0f));

    // Local space transformations
    void setLocalPosition(const glm::vec3& position);
    void setLocalRotation(const glm::quat& rotation);
    void setLocalScale(const glm::vec3& scale);
    void setOwner(SceneNode* node) ;


    // Convenience euler angle rotation
    void setLocalRotationEuler(const glm::vec3& eulerAngles);

    // World space transformations
    glm::vec3 getWorldPosition() const;
    glm::quat getWorldRotation() const;
    glm::vec3 getWorldScale() const;

    // Matrices
    glm::mat4 getLocalMatrix() const;
    glm::mat4 getWorldMatrix() const;

    // Local space getters
    const glm::vec3& getLocalPosition() const { return localPosition; }
    const glm::quat& getLocalRotation() const { return localRotation; }
    const glm::vec3& getLocalScale() const { return localScale; }

    SceneNode* getOwner() const { return owner; }

    // Mark transform as dirty when modified
    void setDirty();

private:
    void updateWorldMatrix() const;

    glm::vec3 localPosition{0.0f};
    glm::quat localRotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 localScale{1.0f};

    mutable glm::mat4 localMatrix{1.0f};
    mutable glm::mat4 worldMatrix{1.0f};
    mutable bool dirty{true};

    SceneNode* owner{nullptr};
};

} // namespace ohao
