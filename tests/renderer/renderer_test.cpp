// Standalone OHAO Renderer Test
// Runs the Vulkan deferred renderer WITHOUT Godot.
// Builds a simple scene, renders, saves to PNG.
//
// Usage: ./renderer_test [output.png]

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "renderer/offscreen/offscreen_renderer.hpp"
#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/material_component.hpp"
#include "renderer/components/light_component.hpp"
#include "renderer/passes/deferred_renderer.hpp"
#include "renderer/camera/camera.hpp"

#include <iostream>
#include <string>
#include <chrono>

using namespace ohao;

// Create a single quad (2 triangles) with given corners and normal, add to model
void addQuad(std::vector<Vertex>& verts, std::vector<uint32_t>& inds,
             glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 normal, glm::vec3 color) {
    uint32_t base = static_cast<uint32_t>(verts.size());
    auto makeVert = [&](glm::vec3 pos) {
        Vertex v{};
        v.position = pos;
        v.normal = normal;
        v.color = color;
        v.texCoord = glm::vec2(0);
        v.tangent = glm::vec4(1,0,0,1);
        v.boneIndices = glm::ivec4(0);
        v.boneWeights = glm::vec4(1,0,0,0);
        return v;
    };
    verts.push_back(makeVert(a));
    verts.push_back(makeVert(b));
    verts.push_back(makeVert(c));
    verts.push_back(makeVert(d));
    inds.push_back(base+0); inds.push_back(base+1); inds.push_back(base+2);
    inds.push_back(base+0); inds.push_back(base+2); inds.push_back(base+3);
}

// Create a wall actor with a custom single-quad mesh (watertight, no gaps)
void addWallQuad(Scene* scene, const std::string& name,
                 glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
                 glm::vec3 normal, glm::vec3 color) {
    auto actor = scene->createActor(name);
    if (!actor) return;

    auto model = std::make_shared<Model>();
    addQuad(model->vertices, model->indices, a, b, c, d, normal, color);

    auto meshComp = actor->addComponent<MeshComponent>();
    meshComp->setModel(model);
    meshComp->setVisible(true);

    auto matComp = actor->addComponent<MaterialComponent>();
    matComp->getMaterial().baseColor = color;
    matComp->getMaterial().roughness = 0.95f;
    matComp->getMaterial().metallic = 0.0f;
}

// Cornell Box — the standard reference scene for GI / RT rendering.
// Tight enclosed room: red left wall, green right wall, white floor/ceiling/back.
// Camera sits just inside the open front face looking in.
std::unique_ptr<Scene> buildTestScene() {
    auto scene = std::make_unique<Scene>("Cornell Box");

    // Box dimensions — the classic Cornell box is roughly a 55cm cube.
    // We scale up for the engine: 5 units per side.
    const float W = 5.0f;   // half-width
    const float H = 5.0f;   // half-height
    const float D = 5.0f;   // half-depth
    const glm::vec3 white(0.73f, 0.73f, 0.73f);
    const glm::vec3 red(0.65f, 0.05f, 0.05f);
    const glm::vec3 green(0.12f, 0.45f, 0.15f);

    // 8 corners of the room — shared by all walls, watertight
    glm::vec3 LBB(-W, -H, -D);  // left-bottom-back
    glm::vec3 RBB( W, -H, -D);  // right-bottom-back
    glm::vec3 LTB(-W,  H, -D);  // left-top-back
    glm::vec3 RTB( W,  H, -D);  // right-top-back
    glm::vec3 LBF(-W, -H,  D);  // left-bottom-front
    glm::vec3 RBF( W, -H,  D);  // right-bottom-front
    glm::vec3 LTF(-W,  H,  D);  // left-top-front
    glm::vec3 RTF( W,  H,  D);  // right-top-front

    // 5 walls as quads — normals face INWARD
    // Back wall (z = -D, normal = +Z)
    addWallQuad(scene.get(), "BackWall",  LBB, RBB, RTB, LTB, glm::vec3(0,0,1), white);
    // Left wall (x = -W, normal = +X) — RED
    addWallQuad(scene.get(), "LeftWall",  LBB, LTB, LTF, LBF, glm::vec3(1,0,0), red);
    // Right wall (x = +W, normal = -X) — GREEN
    addWallQuad(scene.get(), "RightWall", RBB, RBF, RTF, RTB, glm::vec3(-1,0,0), green);
    // Floor (y = -H, normal = +Y)
    addWallQuad(scene.get(), "Floor",     LBB, LBF, RBF, RBB, glm::vec3(0,1,0), white);
    // Ceiling (y = +H, normal = -Y) — emissive light
    addWallQuad(scene.get(), "Ceiling",   LTB, RTB, RTF, LTF, glm::vec3(0,-1,0), white);

    // === Three objects with different materials ===

    // 1. Chrome sphere (metallic, mirror-like) — LEFT-CENTER, larger
    auto metalSphere = scene->createActorWithComponents("MetalSphere", PrimitiveType::Sphere);
    if (metalSphere) {
        metalSphere->getTransform()->setPosition(glm::vec3(-1.8f, -H + 2.0f, 0.0f));
        metalSphere->getTransform()->setScale(glm::vec3(2.0f));
        auto mat = metalSphere->getComponent<MaterialComponent>();
        if (mat) {
            mat->getMaterial().baseColor = glm::vec3(0.95f, 0.93f, 0.88f);  // silver
            mat->getMaterial().roughness = 0.05f;   // mirror-like
            mat->getMaterial().metallic = 1.0f;      // full metal
        }
    }

    // 2. Tall clay block (super rough, matte) — CENTER-RIGHT
    auto clayBlock = scene->createActorWithComponents("ClayBlock", PrimitiveType::Cube);
    if (clayBlock) {
        clayBlock->getTransform()->setPosition(glm::vec3(1.5f, -H + 2.5f, -1.5f));
        clayBlock->getTransform()->setScale(glm::vec3(1.2f, 2.5f, 1.2f));
        clayBlock->getTransform()->setRotation(glm::vec3(0, 20.0f, 0));
        auto mat = clayBlock->getComponent<MaterialComponent>();
        if (mat) {
            mat->getMaterial().baseColor = glm::vec3(0.76f, 0.55f, 0.38f);  // terracotta
            mat->getMaterial().roughness = 0.98f;    // super rough clay
            mat->getMaterial().metallic = 0.0f;
        }
    }

    // 3. Glass-like sphere (glossy, slight tint) — RIGHT-FRONT, larger
    auto glassSphere = scene->createActorWithComponents("GlassSphere", PrimitiveType::Sphere);
    if (glassSphere) {
        glassSphere->getTransform()->setPosition(glm::vec3(2.5f, -H + 1.8f, 1.5f));
        glassSphere->getTransform()->setScale(glm::vec3(1.8f));
        auto mat = glassSphere->getComponent<MaterialComponent>();
        if (mat) {
            mat->getMaterial().baseColor = glm::vec3(0.9f, 0.95f, 1.0f);  // ice blue tint
            mat->getMaterial().roughness = 0.02f;    // super smooth, glossy
            mat->getMaterial().metallic = 0.0f;       // dielectric (glass-like)
        }
    }

    // === Procedural wall painting on the back wall ===
    // Checkerboard pattern made of small colored quads
    float bz = -D + 0.02f;
    float tileSize = 0.8f;
    int gridW = 6, gridH = 4;
    float startX = -(gridW * tileSize) / 2.0f;
    float startY = -0.5f;  // center vertically
    for (int gy = 0; gy < gridH; gy++) {
        for (int gx = 0; gx < gridW; gx++) {
            float x0 = startX + gx * tileSize;
            float y0 = startY + gy * tileSize;
            float x1 = x0 + tileSize * 0.9f;  // small gap between tiles
            float y1 = y0 + tileSize * 0.9f;

            // Procedural color: warm/cool checkerboard
            bool checker = ((gx + gy) % 2) == 0;
            glm::vec3 tileColor;
            if (checker) {
                // Warm tones
                float t = float(gx + gy * gridW) / float(gridW * gridH);
                tileColor = glm::vec3(0.85f - t*0.3f, 0.45f + t*0.2f, 0.25f);
            } else {
                float t = float(gx + gy * gridW) / float(gridW * gridH);
                tileColor = glm::vec3(0.3f, 0.5f + t*0.3f, 0.7f - t*0.2f);
            }

            std::string tileName = "Tile_" + std::to_string(gx) + "_" + std::to_string(gy);
            addWallQuad(scene.get(), tileName,
                glm::vec3(x0, y0, bz), glm::vec3(x1, y0, bz),
                glm::vec3(x1, y1, bz), glm::vec3(x0, y1, bz),
                glm::vec3(0,0,1), tileColor);
        }
    }

    // Ceiling light — point light just below the ceiling, centered
    auto ceilingLight = scene->createActorWithComponents("CeilingLight", PrimitiveType::PointLight);
    if (ceilingLight) {
        ceilingLight->getTransform()->setPosition(glm::vec3(-3.0f, H - 0.5f, -2.0f));
        auto light = ceilingLight->getComponent<LightComponent>();
        if (light) {
            light->setColor(glm::vec3(1.0f, 0.95f, 0.85f));
            light->setIntensity(8.0f);
            light->setRange(15.0f);
        }
    }

    return scene;
}

int main(int argc, char* argv[]) {
    std::string outputPath = "render_output.png";
    if (argc > 1) outputPath = argv[1];

    uint32_t width = 1920;
    uint32_t height = 1080;

    std::cout << "=== OHAO Standalone Renderer Test ===" << std::endl;
    std::cout << "Output: " << outputPath << " (" << width << "x" << height << ")" << std::endl;

    // 1. Create renderer
    OffscreenRenderer renderer(width, height);

    std::cout << "\n--- Initializing renderer ---" << std::endl;
    if (!renderer.initialize()) {
        std::cerr << "FATAL: Failed to initialize renderer" << std::endl;
        return 1;
    }

    // 2. Build scene
    std::cout << "\n--- Building test scene ---" << std::endl;
    auto scene = buildTestScene();
    renderer.setScene(scene.get());

    // Update scene buffers (uploads geometry to GPU, builds RT accel structures)
    if (!renderer.updateSceneBuffers()) {
        std::cerr << "WARNING: No scene meshes uploaded" << std::endl;
    }

    // 3. Camera at the open front of the box, looking in
    // Box is 10 units deep (z=-5 to z=+5). Camera far enough to frame the whole room.
    auto& camera = renderer.getCamera();
    camera.setPosition(glm::vec3(0, 0, 14.0f));
    camera.setFov(33.0f);
    // For rasterization: yaw=-90 means look toward -Z in the engine convention
    // For path tracer: we use the same view matrix
    camera.setRotation(0.0f, 90.0f);  // yaw=90 should look toward -Z in engine convention

    // 4. Path traced mode — full RT, no rasterization
    renderer.setRenderMode(RenderMode::PathTraced);

    // 5. Render — accumulate multiple frames for convergence
    std::cout << "\n--- Path Tracing ---" << std::endl;
    auto start = std::chrono::high_resolution_clock::now();

    int numFrames = 64;  // Low spp for denoiser test
    for (int i = 0; i < numFrames + 3; i++) {
        renderer.render();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Path trace time: " << ms << " ms" << std::endl;

    // Run denoiser on final accumulated HDR, then render one more frame for tonemap
    std::cout << "Running OptiX denoiser..." << std::endl;
    renderer.finalizePathTraced();
    std::cout << "Denoiser done" << std::endl;

    // 6. Save to PNG
    const uint8_t* pixels = renderer.getPixels();
    if (pixels) {
        stbi_write_png(outputPath.c_str(), width, height, 4, pixels, width * 4);
        std::cout << "Saved: " << outputPath << std::endl;
    } else {
        std::cerr << "ERROR: No pixel data" << std::endl;
        return 1;
    }

    // 7. Print RT status
    if (renderer.isRTSupported()) {
        auto* rt = renderer.getRT();
        std::cout << "\n--- RT Status ---" << std::endl;
        std::cout << "BLAS count: " << rt->getBlasCount() << std::endl;
        std::cout << "Instance count: " << rt->getInstanceCount() << std::endl;
        std::cout << "Max recursion depth: " << rt->getPipelineProperties().maxRayRecursionDepth << std::endl;
    } else {
        std::cout << "\nRT: not available" << std::endl;
    }

    // 8. Cleanup
    renderer.shutdown();

    std::cout << "\nDone." << std::endl;
    return 0;
}
