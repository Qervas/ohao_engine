#include "ui/window/window.hpp"
#include "renderer/vulkan_context.hpp"
#include <GLFW/glfw3.h>
#include <chrono>
#include "renderer/camera/camera_controller.hpp"
#include "ui/system/ui_manager.hpp"
#include "ui/selection/selection_manager.hpp"
#include "ui/viewport/viewport_input_handler.hpp"
#include "engine/component/transform_component.hpp"
#include <iostream>
#include <memory>
#include <vulkan/vulkan_core.h>
#include "imgui.h"

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

        // Initialize viewport input handler for edit mode interaction
        ohao::ViewportInputHandler viewportInputHandler;
        viewportInputHandler.initialize(&vulkan, &window, vulkan.getPickingSystem());
        std::cout << "[Main] Viewport input handler initialized" << std::endl;

        auto lastTime = std::chrono::high_resolution_clock::now();
        bool f5Pressed = false;
        bool escPressed = false;
        bool demoPressed = false;

        // Camera focus via double-click handled by ImGui

        while (!window.shouldClose()) {
            auto currentTime = std::chrono::high_resolution_clock::now();
            float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
            lastTime = currentTime;

            window.pollEvents();

            // UE5-style input routing: Set focus state FIRST, then apply to cursor
            // F5 toggles viewport focus mode
            if(window.isKeyPressed(GLFW_KEY_F5)){
                if(!f5Pressed){
                    bool currentlyFocused = uiManager->getViewportToolbar()->isViewportFocused();
                    uiManager->getViewportToolbar()->setViewportFocused(!currentlyFocused);
                    f5Pressed = true;
                }
            }else {
                f5Pressed = false;
            }

            // ESC to exit viewport focus mode
            if(window.isKeyPressed(GLFW_KEY_ESCAPE)){
                if(!escPressed){
                    uiManager->getViewportToolbar()->setViewportFocused(false);
                    escPressed = true;
                }
            }else {
                escPressed = false;
            }

            // Apply viewport focus state to cursor mode IMMEDIATELY
            bool viewportFocused = uiManager->getViewportToolbar()->isViewportFocused();
            window.enableCursor(!viewportFocused); // Disable cursor when focused (for camera control)

            // Update viewport input handler state
            viewportInputHandler.setViewportHovered(uiManager->isSceneViewportHovered());
            viewportInputHandler.setViewportBounds(uiManager->getSceneViewportMin(), uiManager->getSceneViewportMax());
            viewportInputHandler.setPlayMode(viewportFocused);

            // Process edit mode input (when NOT in F5 play mode)
            if (!viewportFocused) {
                viewportInputHandler.update(deltaTime);
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

            // Camera focus on selected object via ImGui double click within viewport
            if (viewportFocused && uiManager->isSceneViewportHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                auto& selectionManager = ohao::SelectionManager::get();
                ohao::Actor* selectedActor = selectionManager.getSelectedActor();
                if (selectedActor) {
                    if (auto* transform = selectedActor->getTransform()) {
                        glm::vec3 targetPosition = transform->getWorldPosition();
                        vulkan.getCamera().focusOnPoint(targetPosition, 5.0f);
                        std::cout << "Camera focused on: " << selectedActor->getName() << std::endl;
                    }
                }
            }

            // Camera controller only runs in F5 play mode (WASD + mouse look)
            // In edit mode, ViewportInputHandler handles camera orbit via right-click
            if (viewportFocused) {
                cameraController.update(deltaTime);
            }

            // Update physics simulation
            vulkan.updateScene(deltaTime);

            uiManager->render();
            vulkan.drawFrame();
            // Exit on Ctrl+Q
            bool ctrlPressed = window.isKeyPressed(GLFW_KEY_LEFT_CONTROL) ||
                             window.isKeyPressed(GLFW_KEY_RIGHT_CONTROL);
            bool qPressed = window.isKeyPressed(GLFW_KEY_Q);
            if(ctrlPressed && qPressed){
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
