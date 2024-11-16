#pragma once
#include <GLFW/glfw3.h>
#include <glm/ext/vector_float2.hpp>
#include <string>
#include <glm/glm.hpp>

namespace ohao {

class Window {
public:
    Window(int w, int h, const std::string& title);
    ~Window();

    bool shouldClose();
    void pollEvents();
    GLFWwindow* getGLFWWindow() const { return window; }

    //input handling
    bool isKeyPressed(int key) const;
    glm::vec2 getMousePosition() const;
    glm::vec2 getMouseDelta();
    void enableCursor(bool enabled);
    void setMousePosition(const glm::vec2& pos);

private:
    GLFWwindow* window;
    glm::vec2 lastMousePos{0.0f};
    bool firstMouse{true};
};

} // namespace ohao
