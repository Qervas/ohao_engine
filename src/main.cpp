#include "ui/window/window.hpp"
#include "ui/components/file_dialog.hpp"
#include "renderer/vulkan_context.hpp"
#include <GLFW/glfw3.h>
#include <chrono>
#include "renderer/camera/camera_controller.hpp"
#include "ui_manager.hpp"
#include <iostream>
#include <vulkan/vulkan_core.h>

int main() {
    try {
        ohao::Window window(1440, 900, "OHAO Engine");
        ohao::VulkanContext vulkan(window.getGLFWWindow());
        vulkan.initializeVulkan();

        ohao::UIManager uiManager(&window, &vulkan);
        uiManager.initialize();
        ohao::CameraController cameraController(vulkan.getCamera(), window, *vulkan.getUniformBuffer());

        auto lastTime = std::chrono::high_resolution_clock::now();

        while (!window.shouldClose()) {
            auto currentTime = std::chrono::high_resolution_clock::now();
            float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
            lastTime = currentTime;

            window.pollEvents();
            cameraController.update(deltaTime);


            // Only update camera if UI isn't capturing input
            if (!uiManager.wantsInputCapture()) {
                cameraController.update(deltaTime);
            }


            uiManager.render();

            vulkan.drawFrame();
            if(window.isKeyPressed(GLFW_KEY_ESCAPE)){
                break;
            }
        }
        vkDeviceWaitIdle(vulkan.getVkDevice());

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
