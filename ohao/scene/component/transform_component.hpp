#pragma once

#include "component.hpp"
#include <glm/glm.hpp>
// Enable experimental GLM features
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <vector>

namespace ohao {

class TransformComponent : public Component {
public:
    using Ptr = std::shared_ptr<TransformComponent>;
    
    TransformComponent();
    ~TransformComponent() override = default;
    
    // Local transform
    void setPosition(const glm::vec3& position);
    void setRotation(const glm::quat& rotation);
    void setRotationEuler(const glm::vec3& eulerAngles);
    void setScale(const glm::vec3& scale);
    void setLocalMatrix(const glm::mat4& matrix);
    
    [[nodiscard]] glm::vec3 getPosition() const;
    [[nodiscard]] glm::quat getRotation() const;
    [[nodiscard]] glm::vec3 getRotationEuler() const;
    [[nodiscard]] glm::vec3 getScale() const;
    [[nodiscard]] glm::mat4 getLocalMatrix() const;
    
    // Relative transformations
    void translate(const glm::vec3& offset);
    void rotate(const glm::quat& rotation);
    void rotateEuler(const glm::vec3& eulerAngles);
    void scaleBy(const glm::vec3& scaleFactors);
    
    // World transform
    [[nodiscard]] glm::mat4 getWorldMatrix() const;
    [[nodiscard]] glm::vec3 getWorldPosition() const;
    [[nodiscard]] glm::quat getWorldRotation() const;
    [[nodiscard]] glm::vec3 getWorldScale() const;
    
    // Directions
    [[nodiscard]] glm::vec3 getForward() const;
    [[nodiscard]] glm::vec3 getRight() const;
    [[nodiscard]] glm::vec3 getUp() const;
    
    // Hierarchy
    void setParent(TransformComponent* parent);
    [[nodiscard]] TransformComponent* getParent() const;
    void addChild(TransformComponent* child);
    void removeChild(TransformComponent* child);
    [[nodiscard]] const std::vector<TransformComponent*>& getChildren() const;
    
    // Mark the transform as dirty (needs recalculation)
    void setDirty();
    [[nodiscard]] bool isDirty() const;
    void clearDirty();
    
    // Component interface
    [[nodiscard]] const char* getTypeName() const override;

private:
    void updateLocalMatrix();
    void updateWorldMatrix();
    
    bool dirty;
    bool worldDirty;
    
    // Local transform
    glm::vec3 position;
    glm::quat rotation;
    glm::vec3 scale;
    glm::mat4 localMatrix;
    
    // World transform
    glm::mat4 worldMatrix;
    
    // Hierarchy
    TransformComponent* parent;
    std::vector<TransformComponent*> children;
};

} // namespace ohao
