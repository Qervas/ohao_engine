#pragma once
#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/vector_float3.hpp>
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace ohao {

class Camera{
public:
    enum class ProjectionType{
        Perspective,
        Orthographic
    };

    Camera(
        float fov = 45.0f,
        float aspect = 16.0f/10.0f,
        float nearPlane = 0.1f,
        float farPlane = 100.0f
    );

    glm::mat4 getViewMatrix() const;
    glm::mat4 getProjectionMatrix() const;
    glm::mat4 getViewProjectionMatrix() const;

    void setPosition(const glm::vec3& position);
    void setRotation(float pitch, float yaw);
    void move(const glm::vec3& offset);
    void rotate(float deltaPitch, float deltaYaw);

    void setProjectionType(ProjectionType type);
    void setPerspectiveProjection(float fov, float aspect, float nearPlane, float farPlane);
    void setOrthographicProjection(float left, float right, float bottom, float top, float nearPlane, float farPlane);

    glm::vec3 getPosition() const {return position;}
    glm::vec3 getFront() const { return front; }
    glm::vec3 getUp() const { return up; }
    glm::vec3 getRight() const { return right; }
    float getPitch() const { return pitch; }
    float getYaw() const { return yaw; }

private:
    void updateVectors();

    glm::vec3 position{0.0f, 0.0f, 3.0f};
    glm::vec3 front{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    glm::vec3 right{1.0f, 0.0f ,0.0f};
    glm::vec3 worldUp{0.0f, 1.0f, 0.0f};

    float pitch{0.0f};
    float yaw{-90.0f};

    ProjectionType projectionType{ProjectionType::Perspective};
    float fov{45.0f};
    float aspectRatio{16.0f/9.0f};
    float nearPlane{0.1f};
    float farPlane{100.0f};

    float orthoLeft{-10.0f};
    float orthoRight{10.0f};
    float orthoBottom{-10.0f};
    float orthoTop{10.0f};

    glm::mat4 viewMatrix{1.0f};
    glm::mat4 projectionMatrix{1.0f};



};

}// namespace ohao
