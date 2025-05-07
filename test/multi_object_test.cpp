#include <gtest/gtest.h>
#include "core/scene/scene.hpp"
#include "core/actor/actor.hpp"
#include "core/components/mesh_component.hpp"
#include "core/components/transform_component.hpp"
#include "ui/selection/selection_manager.hpp"

using namespace ohao;

// This test verifies that multiple objects can exist in a scene
// and can be properly selected and rendered
TEST(MultiObjectTest, CreateAndSelectMultipleActors) {
    // Create a scene
    auto scene = std::make_unique<Scene>();
    ASSERT_TRUE(scene != nullptr);
    
    // Create multiple actors
    auto actor1 = scene->createActor("TestObject1");
    auto actor2 = scene->createActor("TestObject2");
    auto actor3 = scene->createActor("TestObject3");
    
    ASSERT_TRUE(actor1 != nullptr);
    ASSERT_TRUE(actor2 != nullptr);
    ASSERT_TRUE(actor3 != nullptr);
    
    // Verify the actors are stored in the scene correctly
    EXPECT_EQ(scene->getAllActors().size(), 3);
    EXPECT_EQ(actor1->getName(), "TestObject1");
    EXPECT_EQ(actor2->getName(), "TestObject2");
    EXPECT_EQ(actor3->getName(), "TestObject3");
    
    // Position the actors at different positions
    actor1->getTransform()->setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
    actor2->getTransform()->setPosition(glm::vec3(2.0f, 0.0f, 0.0f));
    actor3->getTransform()->setPosition(glm::vec3(-2.0f, 0.0f, 0.0f));
    
    // Verify transforms
    EXPECT_EQ(actor1->getTransform()->getPosition(), glm::vec3(0.0f, 0.0f, 0.0f));
    EXPECT_EQ(actor2->getTransform()->getPosition(), glm::vec3(2.0f, 0.0f, 0.0f));
    EXPECT_EQ(actor3->getTransform()->getPosition(), glm::vec3(-2.0f, 0.0f, 0.0f));
    
    // Set SelectionManager's scene
    SelectionManager::get().setScene(scene.get());
    
    // Test selection
    EXPECT_EQ(SelectionManager::get().getSelectedActor(), nullptr);
    
    // Select actor1
    SelectionManager::get().setSelectedActor(actor1.get());
    EXPECT_EQ(SelectionManager::get().getSelectedActor(), actor1.get());
    EXPECT_TRUE(SelectionManager::get().isSelected(actor1.get()));
    EXPECT_FALSE(SelectionManager::get().isSelected(actor2.get()));
    EXPECT_FALSE(SelectionManager::get().isSelected(actor3.get()));
    
    // Select actor2
    SelectionManager::get().setSelectedActor(actor2.get());
    EXPECT_EQ(SelectionManager::get().getSelectedActor(), actor2.get());
    EXPECT_FALSE(SelectionManager::get().isSelected(actor1.get()));
    EXPECT_TRUE(SelectionManager::get().isSelected(actor2.get()));
    EXPECT_FALSE(SelectionManager::get().isSelected(actor3.get()));
    
    // Test multi-selection
    SelectionManager::get().clearSelection();
    SelectionManager::get().addToSelection(actor1.get());
    SelectionManager::get().addToSelection(actor3.get());
    
    EXPECT_TRUE(SelectionManager::get().isSelected(actor1.get()));
    EXPECT_FALSE(SelectionManager::get().isSelected(actor2.get()));
    EXPECT_TRUE(SelectionManager::get().isSelected(actor3.get()));
    
    // Test clear selection
    SelectionManager::get().clearSelection();
    EXPECT_FALSE(SelectionManager::get().isSelected(actor1.get()));
    EXPECT_FALSE(SelectionManager::get().isSelected(actor2.get()));
    EXPECT_FALSE(SelectionManager::get().isSelected(actor3.get()));
    EXPECT_EQ(SelectionManager::get().getSelectedActor(), nullptr);
}

// This test can't include rendering directly because it requires a Vulkan context,
// but it demonstrates that all the required data structures are properly set up.
TEST(MultiObjectTest, VerifyHierarchyAndComponents) {
    // Create a scene
    auto scene = std::make_unique<Scene>();
    
    // Create a parent actor and a child actor
    auto parent = scene->createActor("Parent");
    auto child = scene->createActor("Child");
    
    // Set up parent-child relationship
    child->setParent(parent.get());
    
    // Verify hierarchy
    EXPECT_EQ(child->getParent(), parent.get());
    EXPECT_EQ(parent->getChildren().size(), 1);
    EXPECT_EQ(parent->getChildren()[0], child.get());
    
    // Add components to both actors
    auto parentMesh = parent->addComponent<MeshComponent>();
    auto childMesh = child->addComponent<MeshComponent>();
    
    // Verify components
    EXPECT_TRUE(parent->hasComponent<MeshComponent>());
    EXPECT_TRUE(child->hasComponent<MeshComponent>());
    EXPECT_EQ(parent->getComponent<MeshComponent>(), parentMesh);
    EXPECT_EQ(child->getComponent<MeshComponent>(), childMesh);
    
    // Verify world transform (child should inherit parent transform)
    parent->getTransform()->setPosition(glm::vec3(1.0f, 0.0f, 0.0f));
    child->getTransform()->setPosition(glm::vec3(0.0f, 1.0f, 0.0f));
    
    // Test local positions
    EXPECT_EQ(parent->getTransform()->getPosition(), glm::vec3(1.0f, 0.0f, 0.0f));
    EXPECT_EQ(child->getTransform()->getPosition(), glm::vec3(0.0f, 1.0f, 0.0f));
    
    // Test world matrices
    glm::vec3 worldPos = glm::vec3(child->getTransform()->getWorldMatrix() * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(worldPos.x, 1.0f, 0.001f);
    EXPECT_NEAR(worldPos.y, 1.0f, 0.001f);
    EXPECT_NEAR(worldPos.z, 0.0f, 0.001f);
} 