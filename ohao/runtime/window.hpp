#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include "gpu/vulkan/present_target.hpp"
#include "runtime/input.hpp"

struct GLFWwindow;

namespace ohao::runtime {

struct WindowConfig {
    uint32_t    width{1920};
    uint32_t    height{1080};
    std::string title{"OHAO"};
    bool        resizable{true};
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
    void setCursorCaptured(bool captured);
    void setSize(uint32_t w, uint32_t h);
    [[nodiscard]] ohao::PresentTarget presentTarget(bool vsync) const;

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
};

} // namespace ohao::runtime
