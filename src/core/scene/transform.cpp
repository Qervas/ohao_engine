#include "transform.hpp"
#include "scene/scene_node.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace ohao {

Transform::Transform(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale)
    : localPosition(position), localRotation(rotation), localScale(scale) {
    setDirty();
}

void Transform::setLocalPosition(const glm::vec3& position) {
    localPosition = position;
    setDirty();
}

void Transform::setLocalRotation(const glm::quat& rotation) {
    localRotation = rotation;
    setDirty();
}

void Transform::setLocalScale(const glm::vec3& scale) {
    localScale = scale;
    setDirty();
}

void Transform::setLocalRotationEuler(const glm::vec3& eulerAngles) {
    localRotation = glm::quat(eulerAngles);
    setDirty();
}

glm::mat4 Transform::getLocalMatrix() const {
    if (dirty) {
        localMatrix = glm::translate(glm::mat4(1.0f), localPosition) *
                     glm::toMat4(localRotation) *
                     glm::scale(glm::mat4(1.0f), localScale);
    }
    return localMatrix;
}

void Transform::setDirty() {
    dirty = true;
}

void Transform::updateWorldMatrix() const {
    if (dirty) {
        localMatrix = glm::translate(glm::mat4(1.0f), localPosition) *
                     glm::toMat4(localRotation) *
                     glm::scale(glm::mat4(1.0f), localScale);

        if (owner && owner->getParent()) {
            worldMatrix = owner->getParent()->getTransform().getWorldMatrix() * localMatrix;
        } else {
            worldMatrix = localMatrix;
        }
        dirty = false;
    }
}

glm::vec3 Transform::getWorldPosition() const {
    updateWorldMatrix();
    return glm::vec3(worldMatrix[3]);
}

glm::quat Transform::getWorldRotation() const {
    updateWorldMatrix();
    return glm::quat_cast(worldMatrix);
}

glm::vec3 Transform::getWorldScale() const {
    updateWorldMatrix();
    return glm::vec3(
        glm::length(worldMatrix[0]),
        glm::length(worldMatrix[1]),
        glm::length(worldMatrix[2])
    );
}

glm::mat4 Transform::getWorldMatrix() const {
    updateWorldMatrix();
    return worldMatrix;
}

} // namespace ohao
