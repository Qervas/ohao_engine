#include "camera.hpp"
#include <cmath>
#include <glm/common.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/vector_float3.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <glm/trigonometric.hpp>
#include <sys/types.h>

namespace ohao{

Camera::Camera(float fov, float aspect, float nearPlane, float farPlane)
    :fov(fov), aspectRatio(aspect), nearPlane(nearPlane), farPlane(farPlane){
        position = glm::vec3(0.0f, 0.0f, 2.5f);
        yaw = -90.0f;
        pitch = 0.0f;
        updateVectors();
    }

void
Camera::updateVectors(){
    // Calculate all vectors first
    glm::vec3 newFront;
    newFront.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    newFront.y = sin(glm::radians(pitch));
    newFront.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
    newFront = glm::normalize(newFront);

    glm::vec3 newRight = glm::normalize(glm::cross(newFront, worldUp));
    glm::vec3 newUp = glm::normalize(glm::cross(newRight, newFront));

    // Update all vectors and matrices atomically
    front = newFront;
    right = newRight;
    up = newUp;

    viewMatrix = glm::lookAt(position, position + front, up);

    if(projectionType == ProjectionType::Perspective) {
        projectionMatrix = glm::perspective(glm::radians(fov), aspectRatio, nearPlane, farPlane);
    } else {
        projectionMatrix = glm::ortho(orthoLeft, orthoRight, orthoBottom, orthoTop, nearPlane, farPlane);
    }
}

glm::mat4
Camera::getViewMatrix() const{
    return viewMatrix;
}

glm::mat4
Camera::getProjectionMatrix() const{
    return projectionMatrix;
}

glm::mat4
Camera::getViewProjectionMatrix() const{
    return projectionMatrix * viewMatrix;
}

void
Camera::setPosition(const glm::vec3& newPosition){
    position = newPosition;
    updateVectors();
}

void
Camera::setRotation(float newPitch, float newYaw){
    pitch = glm::clamp(newPitch, -89.0f, 89.0f);
    yaw = newYaw;
    updateVectors();
}

void
Camera::move(const glm::vec3& offset){
    position += offset;
    updateVectors();
}

void
Camera::rotate(float deltaPitch, float deltaYaw){
    pitch += deltaPitch;
    yaw += deltaYaw;

    pitch = std::clamp(pitch, -89.0f, 89.0f);
    updateVectors();
}

void
Camera::focusOnPoint(const glm::vec3& targetPoint, float distance){
    // Calculate direction from target to camera (opposite of front)
    // This positions the camera behind the target based on current orientation
    glm::vec3 offset = -front * distance;
    glm::vec3 newPosition = targetPoint + offset;

    // Calculate the actual direction from new position to target
    glm::vec3 direction = glm::normalize(targetPoint - newPosition);

    // Convert direction to pitch and yaw
    float newPitch = glm::degrees(asin(direction.y));
    float newYaw = glm::degrees(atan2(direction.z, direction.x));

    // Update camera
    position = newPosition;
    pitch = glm::clamp(newPitch, -89.0f, 89.0f);
    yaw = newYaw;
    updateVectors();
}

void
Camera::setProjectionType(ProjectionType type){
    projectionType = type;
    updateVectors();
}

void
Camera::setPerspectiveProjection(float newFov, float newAspect, float newNear, float newFar){
    fov = newFov;
    aspectRatio = newAspect;
    nearPlane = newNear;
    farPlane = newFar;
    projectionType = ProjectionType::Perspective;
    updateVectors();
}

void
Camera::setOrthographicProjection(float left, float right, float bottom, float top, float newNear, float newFar){
    orthoLeft = left;
    orthoRight = right;
    orthoBottom = bottom;
    orthoTop = top;
    nearPlane = newNear;
    farPlane = newFar;
    projectionType = ProjectionType::Orthographic;
    updateVectors();
}

}
