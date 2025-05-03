#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <cstdint>
#include <glm/ext/vector_float2.hpp>
#include <string>
#include <glm/glm.hpp>

namespace ohao {

class Window {
public:
    Window(uint32_t w, uint32_t h, const std::string& title);
    ~Window();

    bool shouldClose();
    void pollEvents();
    GLFWwindow* getGLFWWindow() const { return window; }

    //input handling
    bool isKeyPressed(int key) const;
    glm::vec2 getMousePosition() const;
    glm::vec2 getMouseDelta();
    uint32_t getWidth() const { return width; }
    uint32_t getHeight() const { return height; }
    void enableCursor(bool enabled);
    void setMousePosition(const glm::vec2& pos);
    void toggleCursorMode();
    bool isCursorEnabled() const {return cursorEnabled;}
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
    bool wasResized();

private:
    GLFWwindow* window;
    glm::vec2 lastMousePos{0.0f};
    bool firstMouse{true};
    uint32_t width{};
    uint32_t height{};
    bool cursorEnabled{true};
    bool framebufferResized{false};
};

} // namespace ohao
