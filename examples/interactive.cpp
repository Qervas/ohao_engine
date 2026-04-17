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
#include "scene/asset/model_loader.hpp"
#include "scene/scene.hpp"
#include "scene/actor/actor.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/component/light_component.hpp"
#include "render/camera/camera.hpp"
#include "render/camera/scene_framer.hpp"

#include <iostream>
#include <string>
#include <chrono>
#include <cmath>

using namespace ohao;

struct CameraState {
    float yaw = -90.0f, pitch = 0.0f;
    glm::vec3 position = {0, 0, 12};  // inside the room (room Z range: -S to +S)
    float speed = 8.0f;
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
    RenderMode rtMode = RenderMode::RTRealtime;
    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "rt_realtime") rtMode = RenderMode::RTRealtime;
        else if (arg == "rt_offline") rtMode = RenderMode::RTOffline;
    }

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

    // Env map: skip for indoor bedroom scene — interior lights only.
    // Outdoor HDR penetrates wall gaps and overexposes the room.
    // Pass "outdoor" as 3rd arg to force env map loading.
    if (!envPath.empty() && argc > 3 && std::string(argv[3]) == "outdoor") {
        renderer.setEnvironmentMap(envPath);
    }

    // Load model
    auto model = modelPath.empty() ? nullptr : ModelLoader::load(modelPath);
    FrameResult frame;
    if (model) {
        frame = SceneFramer::computeFraming(model->vertices);
        std::cout << "Model: " << model->vertices.size() << " verts, scale=" << frame.modelScale << std::endl;
    } else {
        frame.roomHalfSize = 15.0f;
        frame.modelScale = 1.0f;
        frame.modelPosition = glm::vec3(0);
        frame.modelRotation = glm::quat(1, 0, 0, 0);
        frame.cameraPosition = {0, 0, 25};
        frame.cameraFov = 45.0f;
    }
    float S = frame.roomHalfSize;

    // Build bedroom scene
    auto scene = std::make_unique<Scene>("Interactive");

    auto addQuad = [&](const std::string& name, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
                       glm::vec3 normal, glm::vec3 color, float roughness) {
        auto actor = scene->createActor(name);
        auto m = std::make_shared<Model>();
        auto mkv = [&](glm::vec3 pos) {
            Vertex v{}; v.position = pos; v.normal = normal; v.color = color;
            v.texCoord = {0,0}; v.tangent = {1,0,0,1};
            v.boneIndices = glm::ivec4(0); v.boneWeights = {1,0,0,0};
            return v;
        };
        m->vertices = {mkv(a), mkv(b), mkv(c), mkv(d)};
        m->indices = {0,1,2, 0,2,3};
        auto mesh = actor->addComponent<MeshComponent>();
        mesh->setModel(m); mesh->setVisible(true);
        auto mat = actor->addComponent<MaterialComponent>();
        mat->getMaterial().baseColor = color;
        mat->getMaterial().roughness = roughness;
    };

    auto addBox = [&](const std::string& name, glm::vec3 lo, glm::vec3 hi,
                      glm::vec3 color, float roughness) {
        auto actor = scene->createActor(name);
        auto m = std::make_shared<Model>();
        auto mkv = [&](glm::vec3 pos, glm::vec3 n) {
            Vertex v{}; v.position = pos; v.normal = n; v.color = color;
            v.texCoord = {0,0}; v.tangent = {1,0,0,1};
            v.boneIndices = glm::ivec4(0); v.boneWeights = {1,0,0,0};
            return v;
        };
        glm::vec3 c[8] = {
            {lo.x,lo.y,lo.z},{hi.x,lo.y,lo.z},{hi.x,hi.y,lo.z},{lo.x,hi.y,lo.z},
            {lo.x,lo.y,hi.z},{hi.x,lo.y,hi.z},{hi.x,hi.y,hi.z},{lo.x,hi.y,hi.z}
        };
        int faces[6][4] = {{4,5,6,7},{1,0,3,2},{0,4,7,3},{5,1,2,6},{3,7,6,2},{0,1,5,4}};
        glm::vec3 normals[6] = {{0,0,1},{0,0,-1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
        for (int f = 0; f < 6; f++) {
            uint32_t base = (uint32_t)m->vertices.size();
            for (int k = 0; k < 4; k++) m->vertices.push_back(mkv(c[faces[f][k]], normals[f]));
            m->indices.insert(m->indices.end(), {base,base+1,base+2, base,base+2,base+3});
        }
        auto mesh = actor->addComponent<MeshComponent>();
        mesh->setModel(m); mesh->setVisible(true);
        auto mat = actor->addComponent<MaterialComponent>();
        mat->getMaterial().baseColor = color;
        mat->getMaterial().roughness = roughness;
    };

    // Bedroom walls/floor
    glm::vec3 wallColor(0.55f, 0.50f, 0.45f);   // muted warm gray walls
    glm::vec3 floorColor(0.18f, 0.10f, 0.05f);  // dark walnut wood
    glm::vec3 ceilColor(0.60f, 0.58f, 0.55f);   // gray ceiling
    // Walls extend 0.1 past corners to prevent ray leaks at seams
    float E = 0.1f;
    addQuad("Back",   {-S-E,-S-E,-S},{S+E,-S-E,-S},{S+E,S+E,-S},{-S-E,S+E,-S}, {0,0,1},  wallColor, 0.9f);
    addQuad("Front",  {S+E,-S-E,S},{-S-E,-S-E,S},{-S-E,S+E,S},{S+E,S+E,S},     {0,0,-1}, wallColor, 0.9f);
    addQuad("Left",   {-S,-S-E,-S-E},{-S,-S-E,S+E},{-S,S+E,S+E},{-S,S+E,-S-E}, {1,0,0},  wallColor, 0.9f);
    addQuad("Right",  {S,-S-E,S+E},{S,-S-E,-S-E},{S,S+E,-S-E},{S,S+E,S+E},     {-1,0,0}, wallColor, 0.9f);
    addQuad("Floor",  {-S-E,-S,-S-E},{S+E,-S,-S-E},{S+E,-S,S+E},{-S-E,-S,S+E}, {0,1,0},  floorColor, 0.6f);
    addQuad("Ceiling",{-S-E,S,-S-E},{S+E,S,-S-E},{S+E,S,S+E},{-S-E,S,S+E},     {0,-1,0}, ceilColor, 0.9f);

    // Bed + furniture
    float bedW = S*0.8f, bedH = S*0.25f, bedD = S*0.6f;
    addBox("BedFrame", {-bedW,-S,-S}, {bedW,-S+bedH*0.4f,-S+bedD}, {0.20f,0.12f,0.06f}, 0.5f);
    addBox("Mattress", {-bedW+0.2f,-S+bedH*0.4f,-S+0.2f}, {bedW-0.2f,-S+bedH,-S+bedD-0.2f}, {0.85f,0.82f,0.78f}, 0.95f);
    addBox("Headboard", {-bedW,-S,-S}, {bedW,-S+bedH*2.5f,-S+0.3f}, {0.20f,0.12f,0.06f}, 0.4f);
    addBox("Pillow", {-bedW*0.4f,-S+bedH,-S+0.4f}, {bedW*0.4f,-S+bedH+1.0f,-S+bedD*0.4f}, {0.92f,0.90f,0.88f}, 0.95f);
    addBox("Nightstand", {S*0.55f,-S,-S}, {S*0.55f+3.0f,-S+bedH*1.5f,-S+3.0f}, {0.22f,0.14f,0.07f}, 0.45f);

    // Place model
    if (model) {
        auto actor = scene->createActor("Model");
        actor->getTransform()->setRotation(frame.modelRotation);
        actor->getTransform()->setScale(glm::vec3(frame.modelScale));
        actor->getTransform()->setPosition(frame.modelPosition);
        auto mesh = actor->addComponent<MeshComponent>();
        mesh->setModel(model); mesh->setVisible(true);
        auto mat = actor->addComponent<MaterialComponent>();
        mat->getMaterial().baseColor = {0.8f, 0.7f, 0.6f};
        mat->getMaterial().roughness = 0.5f;
    }

    // Bedroom lights
    SceneFramer::applyLights(scene.get(), frame);

    renderer.setScene(scene.get());
    renderer.setRenderMode(rtMode);

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
            renderer.notifyCameraChanged();
            g_cam.moved = false;
        }

        // Path trace
        for (int i = 0; i < g_spp; i++)
            renderer.render();

        // Blit pixels to OpenGL texture → screen
        const uint8_t* pixels = renderer.getPixels();
        if (pixels) {
            // Match GL viewport to actual framebuffer size (handles HiDPI/Wayland scaling)
            int fbW, fbH;
            glfwGetFramebufferSize(window, &fbW, &fbH);
            glViewport(0, 0, fbW, fbH);

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
