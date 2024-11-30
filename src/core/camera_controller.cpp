#include "camera_controller.hpp"
#include <GLFW/glfw3.h>
#include <camera.hpp>
#include <glm/geometric.hpp>

namespace ohao {

CameraController::CameraController(Camera& camera, Window& window, OhaoVkUniformBuffer& uniformBuffer)
    : camera(camera), window(window), uniformBuffer(uniformBuffer) {
}

void
CameraController::update(float deltaTime) {
    updatePosition(deltaTime);
    updateRotation();
}

void
CameraController::updatePosition(float deltaTime){
    float velocity = movementSpeed * deltaTime;
    glm::vec3 movement{0.0f};

    // Forward/Backward
    if (window.isKeyPressed(GLFW_KEY_W))
        movement += camera.getFront();
    if (window.isKeyPressed(GLFW_KEY_S))
        movement -= camera.getFront();

    // Left/Right
    if (window.isKeyPressed(GLFW_KEY_A))
        movement -= camera.getRight();
    if (window.isKeyPressed(GLFW_KEY_D))
        movement += camera.getRight();

    // Up/Down
    if (window.isKeyPressed(GLFW_KEY_SPACE))
        movement += camera.getUp();
    if (window.isKeyPressed(GLFW_KEY_LEFT_CONTROL))
        movement -= camera.getUp();

	// Acceleration
	if (window.isKeyPressed(GLFW_KEY_LEFT_SHIFT))
		velocity *= 4.0f;

    // Normalize and apply movement
    if (glm::length(movement) > 0.0f) {
        movement = glm::normalize(movement) * velocity;
        camera.move( movement);
        uniformBuffer.markForUpdate();
    }

}

void
CameraController::updateRotation(){
    glm::vec2 mouseDelta = window.getMouseDelta();

    if(mouseDelta.x != 0.0f || mouseDelta.y != 0.0f){
        float deltaPitch = mouseDelta.y * mouseSensitivity * (invertY ? 1.0f : -1.0f);
        float deltaYaw = mouseDelta.x * mouseSensitivity;
        camera.rotate(deltaPitch, deltaYaw);
        uniformBuffer.markForUpdate();
    }
}

}
