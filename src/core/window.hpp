#pragma once
#include <GLFW/glfw3.h>
#include <string>

namespace ohao {

class Window {
public:
    Window(int w, int h, const std::string& title);
    ~Window();

    bool shouldClose();
    void pollEvents();

private:
    GLFWwindow* window;
};

} // namespace ohao
