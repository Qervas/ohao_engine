#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include "runtime/window.hpp"
#include <iostream>

namespace ohao::runtime {

static Window* self(GLFWwindow* w) { return static_cast<Window*>(glfwGetWindowUserPointer(w)); }

bool Window::create(const WindowConfig& cfg) {
    if (!glfwInit()) {
        std::cerr << "[Window] glfwInit failed" << std::endl;
        return false;
    }
    if (!glfwVulkanSupported()) {
        std::cerr << "[Window] GLFW found no Vulkan loader" << std::endl;
        glfwTerminate();
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, cfg.resizable ? GLFW_TRUE : GLFW_FALSE);
    m_window = glfwCreateWindow(static_cast<int>(cfg.width), static_cast<int>(cfg.height),
                                cfg.title.c_str(), nullptr, nullptr);
    if (!m_window) {
        std::cerr << "[Window] glfwCreateWindow failed" << std::endl;
        glfwTerminate();
        return false;
    }
    glfwSetWindowUserPointer(m_window, this);
    glfwSetKeyCallback(m_window, onKey);
    glfwSetMouseButtonCallback(m_window, onMouseButton);
    glfwSetCursorPosCallback(m_window, onCursor);
    glfwSetWindowFocusCallback(m_window, onFocus);
    glfwSetFramebufferSizeCallback(m_window, onFramebufferSize);
    return true;
}

void Window::destroy() {
    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
        glfwTerminate();
    }
}

void Window::poll(std::vector<InputEvent>& out) {
    glfwPollEvents();
    out.insert(out.end(), m_pending.begin(), m_pending.end());
    m_pending.clear();
}

bool Window::shouldClose() const { return glfwWindowShouldClose(m_window) != 0; }

void Window::requestClose() { glfwSetWindowShouldClose(m_window, GLFW_TRUE); }

VkExtent2D Window::framebufferExtent() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window, &w, &h);
    return {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
}

bool Window::consumeResized() {
    const bool r = m_resized;
    m_resized = false;
    return r;
}

void Window::setCursorCaptured(bool captured) {
    if (captured == m_cursorCaptured) return;
    m_cursorCaptured = captured;
    glfwSetInputMode(m_window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    // Raw motion only while captured: it bypasses OS acceleration for look input.
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
}

void Window::setSize(uint32_t w, uint32_t h) {
    glfwSetWindowSize(m_window, static_cast<int>(w), static_cast<int>(h));
}

ohao::PresentTarget Window::presentTarget(bool vsync) const {
    ohao::PresentTarget t;
    uint32_t n = 0;
    const char** ext = glfwGetRequiredInstanceExtensions(&n);
    if (ext) t.instanceExtensions.assign(ext, ext + n);
    GLFWwindow* w = m_window;
    t.createSurface = [w](VkInstance inst) {
        VkSurfaceKHR s = VK_NULL_HANDLE;
        if (glfwCreateWindowSurface(inst, w, nullptr, &s) != VK_SUCCESS) s = VK_NULL_HANDLE;
        return s;
    };
    t.extent = framebufferExtent();
    t.vsync = vsync;
    return t;
}

void Window::onKey(GLFWwindow* w, int key, int, int action, int) {
    if (key == GLFW_KEY_UNKNOWN || action == GLFW_REPEAT) return;   // OS repeats are not presses
    InputEvent e;
    e.type = action == GLFW_PRESS ? InputEvent::Type::Down : InputEvent::Type::Up;
    e.device = InputDevice::Key;
    e.code = key;
    self(w)->m_pending.push_back(e);
}

void Window::onMouseButton(GLFWwindow* w, int button, int action, int) {
    InputEvent e;
    e.type = action == GLFW_PRESS ? InputEvent::Type::Down : InputEvent::Type::Up;
    e.device = InputDevice::MouseButton;
    e.code = button;
    self(w)->m_pending.push_back(e);
}

void Window::onCursor(GLFWwindow* w, double x, double y) {
    InputEvent e;
    e.type = InputEvent::Type::MouseMove;
    e.x = x;
    e.y = y;
    self(w)->m_pending.push_back(e);
}

void Window::onFocus(GLFWwindow* w, int focused) {
    if (focused) return;
    InputEvent e;
    e.type = InputEvent::Type::FocusLost;
    self(w)->m_pending.push_back(e);
}

void Window::onFramebufferSize(GLFWwindow* w, int, int) { self(w)->m_resized = true; }

} // namespace ohao::runtime
