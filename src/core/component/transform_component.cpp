#include "transform_component.hpp"
#include "../actor/actor.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace ohao {

TransformComponent::TransformComponent(Actor* owner)
    : Component(owner)
{
}

TransformComponent::~TransformComponent() {
    // Remove from parent if it exists
    removeFromParent();
    
    // Release all children
    for (auto* child : children) {
        child->parent = nullptr;
    }
    children.clear();
}

void TransformComponent::setPosition(const glm::vec3& newPosition) {
    if (position != newPosition) {
        beginModification();
    position = newPosition;
        markMatrixDirty();
        endModification();
    }
}

void TransformComponent::setRotation(const glm::quat& newRotation) {
    if (rotation != newRotation) {
        beginModification();
    rotation = newRotation;
        markMatrixDirty();
        endModification();
}
}

void TransformComponent::setScale(const glm::vec3& newScale) {
    if (scale != newScale) {
        beginModification();
    scale = newScale;
        markMatrixDirty();
        endModification();
    }
}

void TransformComponent::setEulerAngles(const glm::vec3& eulerAngles) {
    // Convert euler angles to quaternion
    setRotation(glm::quat(glm::radians(eulerAngles)));
}

glm::vec3 TransformComponent::getEulerAngles() const {
    // Convert quaternion to euler angles (in degrees)
    return glm::degrees(glm::eulerAngles(rotation));
}

glm::vec3 TransformComponent::getWorldPosition() const {
        return glm::vec3(getWorldMatrix()[3]);
}

glm::quat TransformComponent::getWorldRotation() const {
    if (parent) {
        return parent->getWorldRotation() * rotation;
    }
    return rotation;
}

glm::vec3 TransformComponent::getWorldScale() const {
    if (parent) {
        auto parentScale = parent->getWorldScale();
        return glm::vec3(parentScale.x * scale.x, parentScale.y * scale.y, parentScale.z * scale.z);
    }
    return scale;
}

glm::mat4 TransformComponent::getLocalMatrix() const {
    if (localMatrixDirty) {
        updateLocalMatrix();
    }
    return localMatrix;
}

glm::mat4 TransformComponent::getWorldMatrix() const {
    if (worldMatrixDirty) {
        updateWorldMatrix();
    }
    return worldMatrix;
}

glm::vec3 TransformComponent::getForward() const {
    // Forward is negative Z in OpenGL
    return glm::normalize(glm::rotate(getWorldRotation(), glm::vec3(0.0f, 0.0f, -1.0f)));
}

glm::vec3 TransformComponent::getRight() const {
    // Right is positive X
    return glm::normalize(glm::rotate(getWorldRotation(), glm::vec3(1.0f, 0.0f, 0.0f)));
}

glm::vec3 TransformComponent::getUp() const {
    // Up is positive Y
    return glm::normalize(glm::rotate(getWorldRotation(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

void TransformComponent::setParent(TransformComponent* newParent) {
    if (parent == newParent) return;
    
    beginModification();
    
    // Remove from old parent
    removeFromParent();
    
    // Set new parent
    parent = newParent;
    
    // Add to new parent
    if (parent) {
        parent->addChild(this);
    }
    
    // Mark matrix as dirty
    markMatrixDirty();
    
    endModification();
}

glm::vec3 TransformComponent::transformPoint(const glm::vec3& point) const {
    return glm::vec3(getWorldMatrix() * glm::vec4(point, 1.0f));
}

glm::vec3 TransformComponent::inverseTransformPoint(const glm::vec3& worldPoint) const {
    return glm::vec3(glm::inverse(getWorldMatrix()) * glm::vec4(worldPoint, 1.0f));
}

glm::vec3 TransformComponent::transformDirection(const glm::vec3& direction) const {
    // For directions we don't apply translation
    return glm::vec3(getWorldRotation() * direction);
}

glm::vec3 TransformComponent::inverseTransformDirection(const glm::vec3& worldDirection) const {
    // For directions we don't apply translation
    return glm::vec3(glm::inverse(getWorldRotation()) * worldDirection);
}

void TransformComponent::update(float deltaTime) {
    // Nothing to do in update for transform component
}

const char* TransformComponent::getTypeName() const {
    return "TransformComponent";
}

void TransformComponent::updateLocalMatrix() const {
    glm::mat4 translationMatrix = glm::translate(glm::mat4(1.0f), position);
    glm::mat4 rotationMatrix = glm::toMat4(rotation);
    glm::mat4 scaleMatrix = glm::scale(glm::mat4(1.0f), scale);
    
    localMatrix = translationMatrix * rotationMatrix * scaleMatrix;
    localMatrixDirty = false;
}

void TransformComponent::updateWorldMatrix() const {
    if (parent) {
        worldMatrix = parent->getWorldMatrix() * getLocalMatrix();
    } else {
        worldMatrix = getLocalMatrix();
    }
    worldMatrixDirty = false;
}

void TransformComponent::notifyChildrenOfDirtyMatrix() {
    for (auto* child : children) {
        child->markMatrixDirty();
    }
}

void TransformComponent::removeFromParent() {
    if (parent) {
        parent->removeChild(this);
        parent = nullptr;
    }
}

void TransformComponent::markMatrixDirty() {
    localMatrixDirty = true;
    worldMatrixDirty = true;
    
    // Notify children that their world matrices need to be updated
    notifyChildrenOfDirtyMatrix();
}

void TransformComponent::addChild(TransformComponent* child) {
    if (!child) return;
    
    // Check if already a child
    if (std::find(children.begin(), children.end(), child) == children.end()) {
        children.push_back(child);
    }
}

void TransformComponent::removeChild(TransformComponent* child) {
    if (!child) return;
    
    auto it = std::find(children.begin(), children.end(), child);
    if (it != children.end()) {
        children.erase(it);
    }
}

nlohmann::json TransformComponent::serialize() const {
    nlohmann::json data;
    
    // Store transform properties
    data["position"] = {position.x, position.y, position.z};
    data["rotation"] = {rotation.x, rotation.y, rotation.z, rotation.w};
    data["scale"] = {scale.x, scale.y, scale.z};
    
    return data;
}

void TransformComponent::deserialize(const nlohmann::json& data) {
    beginModification();
    
    // Load transform properties
    if (data.contains("position")) {
        const auto& pos = data["position"];
        position = glm::vec3(pos[0], pos[1], pos[2]);
    }
    
    if (data.contains("rotation")) {
        const auto& rot = data["rotation"];
        rotation = glm::quat(rot[3], rot[0], rot[1], rot[2]);
    }
    
    if (data.contains("scale")) {
        const auto& s = data["scale"];
        scale = glm::vec3(s[0], s[1], s[2]);
    }
    
    markMatrixDirty();
    
    endModification();
}

} // namespace ohao 