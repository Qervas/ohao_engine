#include "transform_component.hpp"
// Enable experimental GLM features
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include "../actor/actor.hpp"
#include "../scene/scene.hpp"

namespace ohao {

TransformComponent::TransformComponent()
    : dirty(true)
    , worldDirty(true)
    , position(0.0f, 0.0f, 0.0f)
    , rotation(1.0f, 0.0f, 0.0f, 0.0f)
    , scale(1.0f, 1.0f, 1.0f)
    , localMatrix(1.0f)
    , worldMatrix(1.0f)
    , parent(nullptr)
{
}

void TransformComponent::setPosition(const glm::vec3& newPosition) {
    position = newPosition;
    setDirty();
}

void TransformComponent::setRotation(const glm::quat& newRotation) {
    rotation = newRotation;
    setDirty();
}

void TransformComponent::setRotationEuler(const glm::vec3& eulerAngles) {
    rotation = glm::quat(eulerAngles);
    setDirty();
}

void TransformComponent::setScale(const glm::vec3& newScale) {
    scale = newScale;
    setDirty();
}

void TransformComponent::setLocalMatrix(const glm::mat4& matrix) {
    localMatrix = matrix;
    
    // Extract components from matrix
    glm::vec3 skew;
    glm::vec4 perspective;
    glm::decompose(matrix, scale, rotation, position, skew, perspective);
    
    setDirty();
}

glm::vec3 TransformComponent::getPosition() const {
    return position;
}

glm::quat TransformComponent::getRotation() const {
    return rotation;
}

glm::vec3 TransformComponent::getRotationEuler() const {
    return glm::eulerAngles(rotation);
}

glm::vec3 TransformComponent::getScale() const {
    return scale;
}

glm::mat4 TransformComponent::getLocalMatrix() const {
    return localMatrix;
}

void TransformComponent::translate(const glm::vec3& offset) {
    position += offset;
    setDirty();
}

void TransformComponent::rotate(const glm::quat& rot) {
    rotation = rotation * rot;
    setDirty();
}

void TransformComponent::rotateEuler(const glm::vec3& eulerAngles) {
    rotate(glm::quat(eulerAngles));
}

void TransformComponent::scaleBy(const glm::vec3& scaleFactors) {
    scale *= scaleFactors;
    setDirty();
}

glm::mat4 TransformComponent::getWorldMatrix() const {
    if (worldDirty) {
        // Update world matrix if needed
        const_cast<TransformComponent*>(this)->updateWorldMatrix();
    }
    return worldMatrix;
}

glm::vec3 TransformComponent::getWorldPosition() const {
    if (parent) {
        return glm::vec3(getWorldMatrix()[3]);
    }
    return position;
}

glm::quat TransformComponent::getWorldRotation() const {
    if (parent) {
        // Extract rotation from world matrix
        glm::vec3 scale;
        glm::quat rotation;
        glm::vec3 translation;
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::decompose(getWorldMatrix(), scale, rotation, translation, skew, perspective);
        return rotation;
    }
    return rotation;
}

glm::vec3 TransformComponent::getWorldScale() const {
    if (parent) {
        // Extract scale from world matrix
        glm::vec3 scale;
        glm::quat rotation;
        glm::vec3 translation;
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::decompose(getWorldMatrix(), scale, rotation, translation, skew, perspective);
        return scale;
    }
    return scale;
}

glm::vec3 TransformComponent::getForward() const {
    return glm::rotate(getWorldRotation(), glm::vec3(0.0f, 0.0f, -1.0f));
}

glm::vec3 TransformComponent::getRight() const {
    return glm::rotate(getWorldRotation(), glm::vec3(1.0f, 0.0f, 0.0f));
}

glm::vec3 TransformComponent::getUp() const {
    return glm::rotate(getWorldRotation(), glm::vec3(0.0f, 1.0f, 0.0f));
}

void TransformComponent::setParent(TransformComponent* newParent) {
    // Remove from old parent
    if (parent && parent != newParent) {
        parent->removeChild(this);
    }
    
    // Update parent
    parent = newParent;
    
    // Add to new parent
    if (parent) {
        parent->addChild(this);
    }
    
    // Mark dirty
    setDirty();
}

TransformComponent* TransformComponent::getParent() const {
    return parent;
}

void TransformComponent::addChild(TransformComponent* child) {
    if (!child) return;
    
    // Check if already child
    auto it = std::find(children.begin(), children.end(), child);
    if (it == children.end()) {
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

const std::vector<TransformComponent*>& TransformComponent::getChildren() const {
    return children;
}

void TransformComponent::setDirty() {
    dirty = true;
    worldDirty = true;
    
    // Mark all children dirty
    for (auto* child : children) {
        if (child) {
            child->setDirty();
        }
    }
    
    // Get actor and mark scene as dirty
    if (auto actor = getOwner()) {
        if (auto scene = actor->getScene()) {
            scene->setDirty();
        }
    }
}

bool TransformComponent::isDirty() const {
    return dirty;
}

const char* TransformComponent::getTypeName() const {
    return "TransformComponent";
}

void TransformComponent::serialize(class Serializer& serializer) const {
    // TODO: Implement serialization
}

void TransformComponent::deserialize(class Deserializer& deserializer) {
    // TODO: Implement deserialization
}

void TransformComponent::updateLocalMatrix() {
    if (dirty) {
        // Build transform matrix: scale -> rotate -> translate
        glm::mat4 scaleMatrix = glm::scale(glm::mat4(1.0f), scale);
        glm::mat4 rotationMatrix = glm::toMat4(rotation);
        glm::mat4 translationMatrix = glm::translate(glm::mat4(1.0f), position);
        
        localMatrix = translationMatrix * rotationMatrix * scaleMatrix;
        dirty = false;
    }
}

void TransformComponent::updateWorldMatrix() {
    // Update local matrix if needed
    if (dirty) {
        updateLocalMatrix();
    }
    
    // Calculate world matrix
    if (parent) {
        worldMatrix = parent->getWorldMatrix() * localMatrix;
    } else {
        worldMatrix = localMatrix;
    }
    
    worldDirty = false;
}

} // namespace ohao 