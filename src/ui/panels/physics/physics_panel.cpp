#include "physics_panel.hpp"
#include "engine/scene/scene.hpp"
#include "ui/components/console_widget.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>

namespace ohao {

PhysicsPanel::PhysicsPanel() : PanelBase("Physics Simulation") {
}

void PhysicsPanel::render() {
    if (!visible) return;

    // Check if we're in a docked/child window context (used by SidePanelManager)
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    bool isInChildWindow = (window && window->ParentWindow != nullptr);

    bool shouldRenderContent = true;

    if (!isInChildWindow) {
        shouldRenderContent = ImGui::Begin("Physics Simulation", &visible, ImGuiWindowFlags_None);
    }

    if (shouldRenderContent) {
        // Main playback controls at the top
        renderPlaybackControls();

        ImGui::Separator();

        // Tabbed interface for different sections
        if (ImGui::BeginTabBar("PhysicsTabBar")) {

            if (ImGui::BeginTabItem("Simulation")) {
                renderSimulationSettings();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("World")) {
                renderWorldSettings();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Debug")) {
                renderDebugTools();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Stats")) {
                renderPerformanceStats();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }

    if (!isInChildWindow) {
        ImGui::End();
    }
}

void PhysicsPanel::renderPlaybackControls() {
    ImGui::Text("Physics Simulation");
    
    // Play/Pause/Stop buttons in a row
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
    
    // Play/Pause button (single button that toggles)
    bool isRunning = (m_simulationState == physics::SimulationState::RUNNING);
    if (isRunning) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.8f, 0.2f, 0.8f));
    }
    
    if (ImGui::Button(isRunning ? "⏸ Pause" : "▶ Play", ImVec2(80, 25))) {
        if (isRunning) {
            m_simulationState = physics::SimulationState::PAUSED;
            OHAO_LOG("Physics simulation paused");
        } else {
            m_simulationState = physics::SimulationState::RUNNING;
            m_singleStepMode = false;  // Exit single-step mode when playing
            OHAO_LOG("Physics simulation started");
        }

        // Sync with physics world
        if (m_physicsWorld) {
            m_physicsWorld->setSimulationState(m_simulationState);
        }
    }
    if (isRunning) {
        ImGui::PopStyleColor();
    }
    
    ImGui::SameLine();
    
    // Stop button
    bool isStopped = (m_simulationState == physics::SimulationState::STOPPED);
    if (isStopped) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 0.8f));
    }
    
    if (ImGui::Button("⏹ Stop", ImVec2(60, 25))) {
        m_simulationState = physics::SimulationState::STOPPED;
        OHAO_LOG("Physics simulation stopped");
        
        // Sync with physics world
        if (m_physicsWorld) {
            m_physicsWorld->setSimulationState(m_simulationState);
        }
    }
    if (isStopped) {
        ImGui::PopStyleColor();
    }
    
    ImGui::SameLine();
    
    // Step button - advances exactly one physics frame
    if (ImGui::Button("⏭ Step", ImVec2(60, 25))) {
        if (m_physicsWorld) {
            m_physicsWorld->stepOnce();  // Step one frame
            m_currentFrame++;
            m_singleStepMode = true;
            OHAO_LOG("Physics stepped to frame " + std::to_string(m_currentFrame));
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Advance simulation by exactly one frame (1/60s)");
    }
    
    ImGui::SameLine();
    
    // Reset button
    if (ImGui::Button("🔄 Reset", ImVec2(60, 25))) {
        m_resetConfirmation = true;
    }
    
    ImGui::PopStyleVar();
    
    // Reset confirmation modal
    if (m_resetConfirmation) {
        ImGui::OpenPopup("Reset Physics?");
    }
    
    if (ImGui::BeginPopupModal("Reset Physics?", &m_resetConfirmation, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Reset all physics objects to initial positions?");
        ImGui::Separator();
        
        if (ImGui::Button("Reset", ImVec2(80, 0))) {
            // Reset physics world to initial state
            OHAO_LOG("Physics simulation reset");
            m_simulationState = physics::SimulationState::STOPPED;
            m_currentFrame = 0;
            m_singleStepMode = false;

            if (m_physicsWorld) {
                m_physicsWorld->reset();  // Reset all bodies to initial positions
                m_physicsWorld->setSimulationState(m_simulationState);
            }

            ImGui::CloseCurrentPopup();
            m_resetConfirmation = false;
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0))) {
            ImGui::CloseCurrentPopup();
            m_resetConfirmation = false;
        }
        ImGui::EndPopup();
    }

    // Frame counter display
    ImGui::Separator();
    ImGui::Text("Frame: %d  |  Time: %.3fs", m_currentFrame, m_currentFrame / 60.0f);
    if (m_singleStepMode) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[STEP MODE]");
    }
    ImGui::Separator();

    // Status and speed on second row
    ImGui::Text("Speed:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("##Speed", &m_simulationSpeed, 0.1f, 5.0f, "%.1fx");
    
    // Quick speed buttons
    ImGui::SameLine();
    if (ImGui::Button("0.5x")) m_simulationSpeed = 0.5f;
    ImGui::SameLine();
    if (ImGui::Button("1x")) m_simulationSpeed = 1.0f;
    ImGui::SameLine();  
    if (ImGui::Button("2x")) m_simulationSpeed = 2.0f;
    
    // Physics enabled checkbox
    ImGui::Checkbox("Physics Enabled", &m_physicsEnabled);
}

void PhysicsPanel::renderSimulationSettings() {
    ImGui::Text("Simulation Settings");
    
    ImGui::Checkbox("Use Fixed Timestep", &m_useFixedTimestep);
    if (m_useFixedTimestep) {
        ImGui::SliderFloat("Timestep", &m_fixedTimestep, 1.0f/120.0f, 1.0f/30.0f, "%.4f s");
        ImGui::Text("Target FPS: %.1f", 1.0f / m_fixedTimestep);
    }
    
    ImGui::SliderInt("Solver Iterations", &m_solverIterations, 1, 50);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Higher = more accurate but slower");
    }
}

void PhysicsPanel::renderWorldSettings() {
    ImGui::Text("Physics World");
    
    ImGui::Text("Gravity:");
    ImGui::SliderFloat3("##Gravity", &m_gravity.x, -20.0f, 20.0f, "%.2f");
    
    // Gravity presets
    if (ImGui::Button("Earth")) {
        m_gravity = glm::vec3(0.0f, -9.81f, 0.0f);
        if (m_physicsWorld) m_physicsWorld->setGravity(m_gravity);
    }
    ImGui::SameLine();
    if (ImGui::Button("Moon")) {
        m_gravity = glm::vec3(0.0f, -1.62f, 0.0f);
        if (m_physicsWorld) m_physicsWorld->setGravity(m_gravity);
    }
    ImGui::SameLine();
    if (ImGui::Button("Zero-G")) {
        m_gravity = glm::vec3(0.0f, 0.0f, 0.0f);
        if (m_physicsWorld) m_physicsWorld->setGravity(m_gravity);
    }
    
    // Apply gravity changes to physics world
    if (m_physicsWorld) {
        m_physicsWorld->setGravity(m_gravity);
    }
}

void PhysicsPanel::renderDebugTools() {
    ImGui::Text("Debug Visualization");
    
    ImGui::Checkbox("Show AABBs", &m_showAABBs);
    ImGui::Checkbox("Show Contact Points", &m_showContacts);
    ImGui::Checkbox("Show Forces", &m_showForces);
    
    if (ImGui::Button("Toggle All")) {
        bool newState = !m_showAABBs;
        m_showAABBs = newState;
        m_showContacts = newState;
        m_showForces = newState;
    }
    
    ImGui::Separator();
    ImGui::Text("Physics Setup");
    
    // Add physics to all objects button
    if (ImGui::Button("Add Physics to All Objects", ImVec2(-1.0f, 0.0f))) {
        if (m_physicsWorld) {
            // Try to get the scene from the physics world context
            // We need to access the scene somehow - let's see if we can get it through a callback
            OHAO_LOG("Adding physics components to all scene objects...");
            
            // For now, we'll add a member to store scene reference
            if (m_scene) {
                m_scene->addPhysicsToAllObjects();
                OHAO_LOG("Physics components added to all objects!");
            } else {
                OHAO_LOG("ERROR: No scene reference available");
            }
        } else {
            OHAO_LOG("ERROR: No physics world connected");
        }
    }
    
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add physics components to all visual objects in the scene.\nThis will make them fall when physics simulation is running.");
    }
    
    // TODO: Connect to debug renderer
}

void PhysicsPanel::renderPerformanceStats() {
    ImGui::Text("Performance Stats");
    
    if (m_physicsWorld) {
        ImGui::Text("Rigid Bodies: %zu", m_physicsWorld->getRigidBodyCount());
        ImGui::Text("Simulation State: %d", static_cast<int>(m_physicsWorld->getSimulationState()));
        ImGui::Text("Panel State: %d", static_cast<int>(m_simulationState));
        
        // DEBUG: Show scene physics components count
        if (m_scene) {
            ImGui::Text("Scene Physics Components: %zu", m_scene->getPhysicsComponents().size());
        }
        
        // TODO: Add more stats from physics world
    } else {
        ImGui::Text("No physics world connected");
    }
    
    // Frame rate info
    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 
                1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
}

void PhysicsPanel::setPhysicsWorld(physics::PhysicsWorld* world) {
    m_physicsWorld = world;
    
    // Sync initial state and gravity
    if (m_physicsWorld) {
        m_physicsWorld->setSimulationState(m_simulationState);
        // Apply the panel's gravity settings to the physics world
        m_physicsWorld->setGravity(m_gravity);
    }
}

} // namespace ohao