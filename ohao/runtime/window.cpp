#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include "runtime/window.hpp"
#include "core/log.hpp"

namespace ohao::runtime {

static Window* self(GLFWwindow* w) { return static_cast<Window*>(glfwGetWindowUserPointer(w)); }

bool Window::create(const WindowConfig& cfg) {
    if (!glfwInit()) {
        OHAO_LOG(Error, runtime, "Window: glfwInit failed");
        return false;
    }
    if (!glfwVulkanSupported()) {
        OHAO_LOG(Error, runtime, "Window: GLFW found no Vulkan loader");
        glfwTerminate();
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, cfg.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, cfg.visible ? GLFW_TRUE : GLFW_FALSE);
    m_window = glfwCreateWindow(static_cast<int>(cfg.width), static_cast<int>(cfg.height),
                                cfg.title.c_str(), nullptr, nullptr);
    if (!m_window) {
        OHAO_LOG(Error, runtime, "Window: glfwCreateWindow failed");
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
    // Motion counts from here: the cursor's jump when it is captured or freed is not a look.
    glfwGetCursorPos(m_window, &m_lastX, &m_lastY);
}

bool Window::focused() const { return glfwGetWindowAttrib(m_window, GLFW_FOCUSED) != 0; }

void Window::setSize(uint32_t w, uint32_t h) {
    glfwSetWindowSize(m_window, static_cast<int>(w), static_cast<int>(h));
}

int Window::refreshRate() const {
    GLFWmonitor* m = m_window ? glfwGetWindowMonitor(m_window) : nullptr;
    if (!m) m = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = m ? glfwGetVideoMode(m) : nullptr;
    return mode ? mode->refreshRate : 0;
}

ohao::PresentTarget Window::presentTarget(ohao::PresentMode mode) const {
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
    t.mode = mode;
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

// Look input comes only from a captured mouse, as relative motion: a free cursor crossing the
// window turns nothing.
void Window::onCursor(GLFWwindow* w, double x, double y) {
    Window* s = self(w);
    const double dx = x - s->m_lastX, dy = y - s->m_lastY;
    s->m_lastX = x;
    s->m_lastY = y;
    if (!s->m_cursorCaptured || (dx == 0.0 && dy == 0.0)) return;
    InputEvent e;
    e.type = InputEvent::Type::MouseDelta;
    e.x = dx;
    e.y = dy;
    s->m_pending.push_back(e);
}

void Window::onFocus(GLFWwindow* w, int focused) {
    if (focused) return;
    InputEvent e;
    e.type = InputEvent::Type::FocusLost;
    self(w)->m_pending.push_back(e);
}

void Window::onFramebufferSize(GLFWwindow* w, int, int) { self(w)->m_resized = true; }

} // namespace ohao::runtime
