#pragma once

#include "component.hpp"
#include <glm/glm.hpp>
// Enable experimental GLM features
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>

namespace ohao {

class TransformComponent : public Component {
public:
    using Ptr = std::shared_ptr<TransformComponent>;
    
    TransformComponent(Actor* owner = nullptr);
    ~TransformComponent() override;
    
    // Local transform properties
    void setPosition(const glm::vec3& position);
    void setRotation(const glm::quat& rotation);
    void setScale(const glm::vec3& scale);
    void setEulerAngles(const glm::vec3& eulerAngles);
    
    const glm::vec3& getPosition() const { return position; }
    const glm::quat& getRotation() const { return rotation; }
    const glm::vec3& getScale() const { return scale; }
    glm::vec3 getEulerAngles() const;
    
    // World transform properties
    glm::vec3 getWorldPosition() const;
    glm::quat getWorldRotation() const;
    glm::vec3 getWorldScale() const;
    
    // Matrices
    glm::mat4 getLocalMatrix() const;
    glm::mat4 getWorldMatrix() const;
    
    // Directions
    glm::vec3 getForward() const;
    glm::vec3 getRight() const;
    glm::vec3 getUp() const;
    
    // Parent-child hierarchy
    void setParent(TransformComponent* parent);
    TransformComponent* getParent() const { return parent; }
    const std::vector<TransformComponent*>& getChildren() const { return children; }
    
    // Coordinate space conversions
    glm::vec3 transformPoint(const glm::vec3& point) const;
    glm::vec3 inverseTransformPoint(const glm::vec3& worldPoint) const;
    glm::vec3 transformDirection(const glm::vec3& direction) const;
    glm::vec3 inverseTransformDirection(const glm::vec3& worldDirection) const;
    
    // Component interface
    void update(float deltaTime) override;
    
    // Type information
    const char* getTypeName() const override;
    static const char* staticTypeName() { return "TransformComponent"; }
    
    // Serialization
    nlohmann::json serialize() const override;
    void deserialize(const nlohmann::json& data) override;
    
private:
    // Local transform
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // Identity quaternion
    glm::vec3 scale{1.0f};
    
    // Cached matrices
    mutable glm::mat4 localMatrix{1.0f};
    mutable glm::mat4 worldMatrix{1.0f};
    mutable bool localMatrixDirty{true};
    mutable bool worldMatrixDirty{true};
    
    // Hierarchy
    TransformComponent* parent{nullptr};
    std::vector<TransformComponent*> children;
    
    // Helper methods
    void updateLocalMatrix() const;
    void updateWorldMatrix() const;
    void notifyChildrenOfDirtyMatrix();
    void removeFromParent();
    void markMatrixDirty();
    void addChild(TransformComponent* child);
    void removeChild(TransformComponent* child);
};

} // namespace ohao 