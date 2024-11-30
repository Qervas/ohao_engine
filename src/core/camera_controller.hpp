#pragma once
#include "camera.hpp"
#include "window.hpp"
#include <glm/glm.hpp>
#include "../renderer/vk/ohao_vk_uniform_buffer.hpp"

namespace ohao {

class CameraController{
public:
    CameraController(Camera& camera, Window& window, OhaoVkUniformBuffer& uniformBuffer);

    void update(float deltaTime);

    float movementSpeed{5.0f};
    float mouseSensitivity{0.1f};
    bool invertY{false};

private:
    void updatePosition(float deltaTime);
    void updateRotation();

    Camera& camera;
    Window& window;
    OhaoVkUniformBuffer& uniformBuffer;
};

}
