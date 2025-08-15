#pragma once

// Physics system temporarily disabled
/*
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace ohao {

// Base class for all collision shapes
class CollisionShape {
public:
    using Ptr = std::shared_ptr<CollisionShape>;
    
    enum class ShapeType {
        BOX,
        SPHERE,
        CAPSULE,
        CONVEX_HULL,
        MESH
    };
    
    CollisionShape(ShapeType type);
    virtual ~CollisionShape() = default;
    
    ShapeType getType() const { return type; }
    
    // Virtual methods for shape-specific operations
    virtual glm::vec3 getCenter() const = 0;
    virtual void setCenter(const glm::vec3& center) = 0;
    
    // Collision detection helpers - to be implemented by derived classes
    virtual bool containsPoint(const glm::vec3& point) const = 0;
    
protected:
    ShapeType type;
    glm::vec3 localCenter{0.0f}; // Center in local space
};

// Box shape - AABB in local space
class BoxShape : public CollisionShape {
public:
    BoxShape(const glm::vec3& halfExtents);
    ~BoxShape() override = default;
    
    const glm::vec3& getHalfExtents() const { return halfExtents; }
    void setHalfExtents(const glm::vec3& extents) { halfExtents = extents; }
    
    // CollisionShape interface implementation
    glm::vec3 getCenter() const override { return localCenter; }
    void setCenter(const glm::vec3& center) override { localCenter = center; }
    bool containsPoint(const glm::vec3& point) const override;
    
private:
    glm::vec3 halfExtents;
};

// Sphere shape
class SphereShape : public CollisionShape {
public:
    SphereShape(float radius);
    ~SphereShape() override = default;
    
    float getRadius() const { return radius; }
    void setRadius(float r) { radius = r; }
    
    // CollisionShape interface implementation
    glm::vec3 getCenter() const override { return localCenter; }
    void setCenter(const glm::vec3& center) override { localCenter = center; }
    bool containsPoint(const glm::vec3& point) const override;
    
private:
    float radius;
};

// Capsule shape
class CapsuleShape : public CollisionShape {
public:
    CapsuleShape(float radius, float height);
    ~CapsuleShape() override = default;
    
    float getRadius() const { return radius; }
    void setRadius(float r) { radius = r; }
    
    float getHeight() const { return height; }
    void setHeight(float h) { height = h; }
    
    // CollisionShape interface implementation
    glm::vec3 getCenter() const override { return localCenter; }
    void setCenter(const glm::vec3& center) override { localCenter = center; }
    bool containsPoint(const glm::vec3& point) const override;
    
private:
    float radius;
    float height;
};

// Convex hull shape
class ConvexHullShape : public CollisionShape {
public:
    ConvexHullShape(const std::vector<glm::vec3>& points);
    ~ConvexHullShape() override = default;
    
    const std::vector<glm::vec3>& getPoints() const { return points; }
    
    // CollisionShape interface implementation
    glm::vec3 getCenter() const override { return localCenter; }
    void setCenter(const glm::vec3& center) override { localCenter = center; }
    bool containsPoint(const glm::vec3& point) const override;
    
private:
    std::vector<glm::vec3> points;
};

} // namespace ohao
*/