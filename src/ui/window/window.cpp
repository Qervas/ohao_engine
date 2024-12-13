#include "window.hpp"
#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include <cstdint>
#include <stdexcept>

namespace ohao {

Window::Window(uint32_t w, uint32_t h, const std::string& title)
    : width(w), height(h){
    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialize GLFW");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        throw std::runtime_error("Failed to create window");
    }
    enableCursor(true);
    glfwSetWindowUserPointer(window, this);
    glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);
}

void Window::framebufferResizeCallback(GLFWwindow* window, int width, int height){
    auto app = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    app->width = width;
    app->height = height;
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
    glfwSetInputMode(window, GLFW_CURSOR,
        enabled ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
    firstMouse = true;
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
