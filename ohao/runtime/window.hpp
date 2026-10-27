#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include "gpu/vulkan/present_target.hpp"
#include "core/input.hpp"

struct GLFWwindow;

namespace ohao::runtime {

struct WindowConfig {
    uint32_t    width{1920};
    uint32_t    height{1080};
    std::string title{"OHAO"};
    bool        resizable{true};
    bool        visible{true};   // false: never shown (tests); it still presents
};

// A GLFW window with no graphics context of its own: the renderer presents into
// it through a Vulkan surface. Input arrives as InputEvents; GLFW never leaks
// past this class.
class Window {
public:
    Window() = default;
    ~Window() { destroy(); }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool create(const WindowConfig& cfg);
    void destroy();
    // Pumps the OS event queue and appends this frame's input to `out`.
    void poll(std::vector<InputEvent>& out);
    [[nodiscard]] bool shouldClose() const;
    void requestClose();
    // Pixel size of the drawable area; 0x0 while minimized.
    [[nodiscard]] VkExtent2D framebufferExtent() const;
    // True once after each framebuffer size change.
    [[nodiscard]] bool consumeResized();
    // Captured: hidden and held by the window, its motion reported as MouseDelta events (look
    // input). Not captured: the cursor is free and its motion is not reported.
    void setCursorCaptured(bool captured);
    [[nodiscard]] bool cursorCaptured() const noexcept { return m_cursorCaptured; }
    [[nodiscard]] bool focused() const;
    void setSize(uint32_t w, uint32_t h);
    [[nodiscard]] ohao::PresentTarget presentTarget(ohao::PresentMode mode) const;
    // The refresh rate of the display the window is on (the primary display while windowed), Hz;
    // 0 when unknown.
    [[nodiscard]] int refreshRate() const;

private:
    static void onKey(GLFWwindow*, int key, int scancode, int action, int mods);
    static void onMouseButton(GLFWwindow*, int button, int action, int mods);
    static void onCursor(GLFWwindow*, double x, double y);
    static void onFocus(GLFWwindow*, int focused);
    static void onFramebufferSize(GLFWwindow*, int w, int h);

    GLFWwindow* m_window{nullptr};
    std::vector<InputEvent> m_pending;
    bool m_resized{false};
    bool m_cursorCaptured{false};
    double m_lastX{0.0}, m_lastY{0.0};   // the cursor at its last event (or capture change)
};

} // namespace ohao::runtime
