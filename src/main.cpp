#include "ui/window/window.hpp"
#include "renderer/vulkan_context.hpp"
#include <GLFW/glfw3.h>
#include <chrono>
#include "renderer/camera/camera_controller.hpp"
#include "ui/system/ui_manager.hpp"
#include <iostream>
#include <memory>
#include <vulkan/vulkan_core.h>

// Forward declaration of demo functions
namespace ohao {
    void runMultiObjectDemo(VulkanContext* context);
}

int main() {
    try {
        ohao::Window window("OHAO Engine");
        ohao::VulkanContext vulkan(&window);
        vulkan.initializeVulkan();

        auto uiManager = std::make_shared<ohao::UIManager>(&window, &vulkan);
        vulkan.setUIManager(uiManager);
        uiManager->initialize();
        vulkan.initializeSceneRenderer();
        ohao::CameraController cameraController(vulkan.getCamera(), window, *vulkan.getUniformBuffer());

        auto lastTime = std::chrono::high_resolution_clock::now();
        bool tabPressed = false;
        bool demoPressed = false;

        while (!window.shouldClose()) {
            auto currentTime = std::chrono::high_resolution_clock::now();
            float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
            lastTime = currentTime;

            window.pollEvents();
            cameraController.update(deltaTime);

            if(window.isKeyPressed(GLFW_KEY_TAB)){
                if(!tabPressed){
                    window.toggleCursorMode();
                    tabPressed = true;
                }
            }else {
                tabPressed = false;
            }

            // Check for the M key to load multi-object demo
            if(window.isKeyPressed(GLFW_KEY_M)){
                if(!demoPressed){
                    std::cout << "Loading multi-object demo (press M)" << std::endl;
                    ohao::runMultiObjectDemo(&vulkan);
                    demoPressed = true;
                }
            }else {
                demoPressed = false;
            }

            if (!uiManager->wantsInputCapture()) {
                cameraController.update(deltaTime);
            }

            uiManager->render();
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
