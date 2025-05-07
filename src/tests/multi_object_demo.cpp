#include <iostream>
#include <memory>
#include <string>

#include "core/actor/actor.hpp"
#include "core/component/mesh_component.hpp"
#include "core/component/transform_component.hpp"
#include "core/scene/scene.hpp"
#include "utils/common_types.hpp"
#include "renderer/vulkan_context.hpp"

namespace ohao {

// This is a demo function that can be called from main.cpp to showcase the multi-object rendering system
void runMultiObjectDemo(VulkanContext* context) {
    std::cout << "Running Multi-Object Demo...\n";
    
    // Create a new scene in the context
    context->createNewScene("Multi-Object Demo");
    
    // Get the scene from the context
    auto* scene = context->getScene();
    if (!scene) {
        std::cerr << "Failed to create scene for demo\n";
        return;
    }
    
    // Create multiple actors at different positions
    auto cube1 = scene->createActor("Cube1");
    auto cube2 = scene->createActor("Cube2");
    auto cube3 = scene->createActor("Cube3");
    
    // Position them in a triangle formation with clear positions
    // Put all objects closer to the camera
    cube1->getTransform()->setPosition(glm::vec3(-2.0f, 0.0f, 0.0f));  // Left
    cube2->getTransform()->setPosition(glm::vec3(2.0f, 0.0f, 0.0f));   // Right
    cube3->getTransform()->setPosition(glm::vec3(0.0f, 0.0f, -4.0f));  // Center back
    
    // Scale them differently for better visibility
    cube1->getTransform()->setScale(glm::vec3(1.0f, 1.0f, 1.0f));      // Normal size
    cube2->getTransform()->setScale(glm::vec3(1.5f, 1.5f, 1.5f));      // Larger
    cube3->getTransform()->setScale(glm::vec3(2.0f, 2.0f, 2.0f));      // Largest
    
    // Add mesh components
    auto mesh1 = cube1->addComponent<MeshComponent>();
    auto mesh2 = cube2->addComponent<MeshComponent>();
    auto mesh3 = cube3->addComponent<MeshComponent>();
    
    // Create sample model with proper normals and indexing
    auto cubeModel = std::make_shared<Model>();
    
    // Cube vertices with proper normals for each face
    std::vector<Vertex> vertices = {
        // Front face - normals pointing toward +Z
        {{-0.5f, -0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{0.5f, -0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{-0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        
        // Back face - normals pointing toward -Z
        {{-0.5f, -0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
        {{0.5f, -0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
        {{0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
        {{-0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
        
        // Left face - normals pointing toward -X
        {{-0.5f, -0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}}, 
        {{-0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{-0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},
        
        // Right face - normals pointing toward +X
        {{0.5f, -0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{0.5f, -0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},
        
        // Top face - normals pointing toward +Y
        {{-0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
        {{-0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
        
        // Bottom face - normals pointing toward -Y
        {{-0.5f, -0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
        {{0.5f, -0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},
        {{0.5f, -0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
        {{-0.5f, -0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}}
    };
    
    // Explicitly define each face with proper winding
    std::vector<uint32_t> indices = {
        // Front face
        0, 1, 2, 2, 3, 0,
        // Back face
        4, 5, 6, 6, 7, 4,
        // Left face
        8, 9, 10, 10, 11, 8,
        // Right face
        12, 13, 14, 14, 15, 12,
        // Top face
        16, 17, 18, 18, 19, 16,
        // Bottom face
        20, 21, 22, 22, 23, 20
    };
    
    cubeModel->vertices = vertices;
    cubeModel->indices = indices;
    
    // Set different colors for the cubes to make them distinguishable
    auto model1 = std::make_shared<Model>(*cubeModel);
    auto model2 = std::make_shared<Model>(*cubeModel);
    auto model3 = std::make_shared<Model>(*cubeModel);
    
    // Set bright red color for cube1
    for (auto& vertex : model1->vertices) {
        vertex.color = {1.0f, 0.0f, 0.0f}; // Bright Red
    }
    
    // Set bright green color for cube2
    for (auto& vertex : model2->vertices) {
        vertex.color = {0.0f, 1.0f, 0.0f}; // Bright Green
    }
    
    // Set bright blue color for cube3
    for (auto& vertex : model3->vertices) {
        vertex.color = {0.0f, 0.0f, 1.0f}; // Bright Blue
    }
    
    // Assign the colored models
    mesh1->setModel(model1);
    mesh2->setModel(model2);
    mesh3->setModel(model3);
    
    // Update scene buffers to include all models
    context->updateSceneBuffers();
    
    // Set camera position to see all cubes
    context->getCamera().setPosition(glm::vec3(0.0f, 2.0f, 5.0f));
    context->getCamera().setRotation(-15.0f, -90.0f);  // Look down slightly at the scene
    
    std::cout << "Multi-Object Demo initialized with " << scene->getAllActors().size() << " actors.\n";
    std::cout << "You should now be able to see and select three different colored cubes in the viewport.\n";
}

} // namespace ohao 