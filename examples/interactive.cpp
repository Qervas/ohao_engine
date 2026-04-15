// Interactive Path Tracer Viewer — GLFW + OpenGL display + Vulkan RT
// Usage: ./interactive [model.glb] [env.hdr]
//
// Controls:
//   WASD  — move camera
//   Mouse — look around (hold right click)
//   +/-   — increase/decrease spp per frame
//   F12   — save screenshot
//   ESC   — quit

#include <GL/gl.h>
#include <GLFW/glfw3.h>

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "gpu/vulkan/renderer.hpp"
#include "scene/scene.hpp"
#include "scene/actor/actor.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/component/light_component.hpp"
#include "render/camera/camera.hpp"

#include <iostream>
#include <string>
#include <chrono>
#include <cmath>

using namespace ohao;

struct CameraState {
    float yaw = -90.0f, pitch = 0.0f;
    glm::vec3 position = {0, 0, 8};
    float speed = 5.0f;
    float sensitivity = 0.15f;
    bool rightMouseDown = false;
    double lastMouseX = 0, lastMouseY = 0;
    bool moved = true;  // start true to trigger initial accumulation reset
};

static CameraState g_cam;
static int g_spp = 1;

void keyCallback(GLFWwindow* window, int key, int, int action, int) {
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    if (key == GLFW_KEY_EQUAL && action == GLFW_PRESS) g_spp = std::min(g_spp * 2, 64);
    if (key == GLFW_KEY_MINUS && action == GLFW_PRESS) g_spp = std::max(g_spp / 2, 1);
}

void mouseButtonCallback(GLFWwindow* window, int button, int action, int) {
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        g_cam.rightMouseDown = (action == GLFW_PRESS);
        if (g_cam.rightMouseDown) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            glfwGetCursorPos(window, &g_cam.lastMouseX, &g_cam.lastMouseY);
        } else {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
    }
}

void cursorCallback(GLFWwindow*, double xpos, double ypos) {
    if (!g_cam.rightMouseDown) return;
    float dx = static_cast<float>(xpos - g_cam.lastMouseX) * g_cam.sensitivity;
    float dy = static_cast<float>(g_cam.lastMouseY - ypos) * g_cam.sensitivity;
    g_cam.lastMouseX = xpos;
    g_cam.lastMouseY = ypos;
    g_cam.yaw += dx;
    g_cam.pitch = std::clamp(g_cam.pitch + dy, -89.0f, 89.0f);
    g_cam.moved = true;
}

void processInput(GLFWwindow* window, float dt) {
    glm::vec3 front;
    front.x = cos(glm::radians(g_cam.yaw)) * cos(glm::radians(g_cam.pitch));
    front.y = sin(glm::radians(g_cam.pitch));
    front.z = sin(glm::radians(g_cam.yaw)) * cos(glm::radians(g_cam.pitch));
    front = glm::normalize(front);
    glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0, 1, 0)));

    float spd = g_cam.speed * dt;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) spd *= 3.0f;

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) { g_cam.position += front * spd; g_cam.moved = true; }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) { g_cam.position -= front * spd; g_cam.moved = true; }
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) { g_cam.position -= right * spd; g_cam.moved = true; }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) { g_cam.position += right * spd; g_cam.moved = true; }
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) { g_cam.position.y -= spd; g_cam.moved = true; }
    if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) { g_cam.position.y += spd; g_cam.moved = true; }
}

int main(int argc, char* argv[]) {
    std::string modelPath = argc > 1 ? argv[1] : "";
    std::string envPath = argc > 2 ? argv[2] : "";
    uint32_t W = 1280, H = 720;

    // Init GLFW with OpenGL (for pixel display)
    if (!glfwInit()) { std::cerr << "GLFW init failed" << std::endl; return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(W, H, "OHAO Interactive RT", nullptr, nullptr);
    if (!window) { std::cerr << "Window failed" << std::endl; glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);  // no vsync — show max fps
    glfwSetKeyCallback(window, keyCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetCursorPosCallback(window, cursorCallback);

    // Create OpenGL texture for pixel display
    GLuint displayTex;
    glGenTextures(1, &displayTex);
    glBindTexture(GL_TEXTURE_2D, displayTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    std::cout << "=== OHAO Interactive Path Tracer ===" << std::endl;
    std::cout << "WASD=move  RightMouse=look  +/-=spp  F12=screenshot  ESC=quit" << std::endl;

    // Init Vulkan renderer (offscreen)
    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) { std::cerr << "Renderer init failed" << std::endl; return 1; }

    if (!envPath.empty()) renderer.setEnvironmentMap(envPath);

    // Build scene
    auto scene = std::make_unique<Scene>("Interactive");

    if (!modelPath.empty()) {
        auto model = std::make_shared<Model>();
        bool loaded = false;
        auto dot = modelPath.find_last_of('.');
        std::string ext = (dot != std::string::npos) ? modelPath.substr(dot + 1) : "";
        if (ext == "obj") loaded = model->loadFromOBJ(modelPath);
        else loaded = model->loadFromGLTF(modelPath);

        if (loaded) {
            glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
            for (const auto& v : model->vertices) {
                bmin = glm::min(bmin, v.position);
                bmax = glm::max(bmax, v.position);
            }
            glm::vec3 extent = bmax - bmin;
            bool isYUp = (extent.y >= extent.z);
            float modelHeight = isYUp ? extent.y : extent.z;
            float scale = 4.0f / modelHeight;

            auto actor = scene->createActor("Model");
            if (isYUp)
                actor->getTransform()->setRotation(glm::quat(glm::radians(glm::vec3(0, 180, 0))));
            else
                actor->getTransform()->setRotation(glm::quat(glm::radians(glm::vec3(-90, 0, 0))));
            actor->getTransform()->setScale(glm::vec3(scale));
            glm::vec3 center = (bmin + bmax) * 0.5f;
            actor->getTransform()->setPosition({-center.x*scale, -center.y*scale, -center.z*scale});
            auto mesh = actor->addComponent<MeshComponent>();
            mesh->setModel(model); mesh->setVisible(true);
            actor->addComponent<MaterialComponent>();
            std::cout << "Model: " << model->vertices.size() << " verts" << std::endl;
        }
    }

    auto keyLight = scene->createActor("Key");
    auto kl = keyLight->addComponent<LightComponent>();
    kl->setLightType(LightType::Sphere);
    kl->setColor({1, 0.95f, 0.9f});
    kl->setIntensity(15.0f);
    kl->setRadius(1.0f);
    keyLight->getTransform()->setPosition({4, 3, 4});

    renderer.setScene(scene.get());
    renderer.setRenderMode(RenderMode::PathTraced);

    // Main loop
    auto lastTime = std::chrono::high_resolution_clock::now();
    int frameCount = 0;
    float fpsTimer = 0;

    while (!glfwWindowShouldClose(window)) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        glfwPollEvents();
        processInput(window, dt);

        // Update camera
        auto& camera = renderer.getCamera();
        camera.setPosition(g_cam.position);
        camera.setRotation(g_cam.pitch, g_cam.yaw);
        camera.setFov(45.0f);

        if (g_cam.moved) {
            renderer.resetAccumulation();
            g_cam.moved = false;
        }

        // Path trace
        for (int i = 0; i < g_spp; i++)
            renderer.render();

        // Blit pixels to OpenGL texture → screen
        const uint8_t* pixels = renderer.getPixels();
        if (pixels) {
            glBindTexture(GL_TEXTURE_2D, displayTex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

            // Draw fullscreen quad with the texture
            glEnable(GL_TEXTURE_2D);
            glBegin(GL_QUADS);
            glTexCoord2f(0, 1); glVertex2f(-1, -1);
            glTexCoord2f(1, 1); glVertex2f( 1, -1);
            glTexCoord2f(1, 0); glVertex2f( 1,  1);
            glTexCoord2f(0, 0); glVertex2f(-1,  1);
            glEnd();
        }

        glfwSwapBuffers(window);

        // FPS counter
        frameCount++;
        fpsTimer += dt;
        if (fpsTimer >= 1.0f) {
            float fps = frameCount / fpsTimer;
            int totalSpp = renderer.getPathTracerFrameIndex();
            char title[256];
            snprintf(title, sizeof(title), "OHAO Interactive RT | %.1f fps | %d spp/frame | %d total spp",
                     fps, g_spp, totalSpp);
            glfwSetWindowTitle(window, title);
            frameCount = 0;
            fpsTimer = 0;
        }

        // Screenshot
        static bool f12Pressed = false;
        if (glfwGetKey(window, GLFW_KEY_F12) == GLFW_PRESS && !f12Pressed) {
            f12Pressed = true;
            if (pixels) {
                stbi_write_png("renders/screenshot.png", W, H, 4, pixels, W * 4);
                std::cout << "Screenshot: renders/screenshot.png" << std::endl;
            }
        }
        if (glfwGetKey(window, GLFW_KEY_F12) == GLFW_RELEASE) f12Pressed = false;
    }

    scene.reset();  // destroy scene before renderer to avoid cleanup crash
    glDeleteTextures(1, &displayTex);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
