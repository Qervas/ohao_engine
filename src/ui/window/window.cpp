#include "window.hpp"
#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include <cstdint>
#include <stdexcept>

namespace ohao {


Window::Window(const std::string& title)
    : width(0), height(0), framebufferResized(false), cursorEnabled(true), firstMouse(true) {
    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialize GLFW");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // Get primary monitor
    GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();
    if (!primaryMonitor) {
        glfwTerminate();
        throw std::runtime_error("Failed to get primary monitor");
    }

    // Get the work area of the primary monitor
    int workareaX, workareaY, workareaWidth, workareaHeight;
    glfwGetMonitorWorkarea(primaryMonitor, &workareaX, &workareaY, &workareaWidth, &workareaHeight);

    this->width = static_cast<uint32_t>(workareaWidth);
    this->height = static_cast<uint32_t>(workareaHeight);

    // Create the window with the work area dimensions
    window = glfwCreateWindow(this->width, this->height, title.c_str(), nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        throw std::runtime_error("Failed to create window");
    }

    // Position the window at the top-left of the work area and maximize it
    glfwSetWindowPos(window, workareaX, workareaY);
    glfwMaximizeWindow(window);

    int currentFbWidth, currentFbHeight;
    glfwGetFramebufferSize(window, &currentFbWidth, &currentFbHeight);
    this->width = static_cast<uint32_t>(currentFbWidth);
    this->height = static_cast<uint32_t>(currentFbHeight);
    // We don't set framebufferResized = true here, because this is the initial setup.
    // The VulkanContext will read these final width/height values.

    enableCursor(true); // Set initial cursor state
    glfwSetWindowUserPointer(window, this);
    glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);
}

void Window::framebufferResizeCallback(GLFWwindow* window, int width, int height){
    auto app = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    app->width = static_cast<uint32_t>(width); // Ensure cast
    app->height = static_cast<uint32_t>(height); // Ensure cast
    app->framebufferResized = true;
}

Window::~Window() {
    if (window) {
        glfwDestroyWindow(window);
        window = nullptr;
    }
    glfwTerminate();
}

bool Window::shouldClose() {
    return glfwWindowShouldClose(window);
}

void Window::pollEvents() {
    glfwPollEvents();
}

bool Window::isKeyPressed(int key) const{
    return glfwGetKey(window, key) == GLFW_PRESS;
}

bool Window::isMouseButtonPressed(int button) const{
    return glfwGetMouseButton(window, button) == GLFW_PRESS;
}

glm::vec2 Window::getMousePosition() const{
    double xpos, ypos;
    glfwGetCursorPos(window, &xpos, &ypos);
    return glm::vec2(xpos, ypos);
}

glm::vec2 Window::getMouseDelta(){
    glm::vec2 currentPos = getMousePosition();
    if(firstMouse){
        lastMousePos = currentPos;
        firstMouse = false;
        return glm::vec2(0.0f);
    }

    glm::vec2 delta = currentPos - lastMousePos;
    lastMousePos = currentPos;
    return delta;
}

void
Window::enableCursor(bool enabled){
    if (cursorEnabled == enabled) {
        return;
    }
    glfwSetInputMode(window, GLFW_CURSOR,
        enabled ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
    firstMouse = true;
    cursorEnabled = enabled;
}

void Window::setMousePosition(const glm::vec2& pos){
    glfwSetCursorPos(window, pos.x, pos.y);
    lastMousePos = pos;
}

void Window::toggleCursorMode(){
    cursorEnabled = ! cursorEnabled;
    enableCursor(cursorEnabled);
}

bool Window::wasResized(){
    bool resized = framebufferResized;
    framebufferResized = false;
    return resized;
}


} // namespace ohao
