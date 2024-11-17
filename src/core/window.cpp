#include "window.hpp"
#include <GLFW/glfw3.h>
#include <stdexcept>

namespace ohao {

Window::Window(int w, int h, const std::string& title) {
    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialize GLFW");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    window = glfwCreateWindow(w, h, title.c_str(), nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        throw std::runtime_error("Failed to create window");
    }
    enableCursor(false);
}

Window::~Window() {
    if (window) {
        glfwDestroyWindow(window);
        window = nullptr;
    }
    glfwTerminate();
}

bool
Window::shouldClose() {
    return glfwWindowShouldClose(window);
}

void
Window::pollEvents() {
    glfwPollEvents();
}

bool
Window::isKeyPressed(int key) const{
    return glfwGetKey(window, key) == GLFW_PRESS;
}

glm::vec2
Window::getMousePosition() const{
    double xpos, ypos;
    glfwGetCursorPos(window, &xpos, &ypos);
    return glm::vec2(xpos, ypos);
}

glm::vec2
Window::getMouseDelta(){
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

void
Window::setMousePosition(const glm::vec2& pos){
    glfwSetCursorPos(window, pos.x, pos.y);
    lastMousePos = pos;
}

} // namespace ohao