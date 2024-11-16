#pragma once
#include "camera.hpp"
#include "window.hpp"
#include <glm/glm.hpp>

namespace ohao {

class CameraController{
public:
    CameraController(Camera& camera, Window& window);

    void update(float deltaTime);

    float movementSpeed{5.0f};
    float mouseSensitivity{0.1f};
    bool invertY{false};

private:
    void updatePosition(float deltaTime);
    void updateRotation();

    Camera& camera;
    Window& window;
};

}
