// Standalone OHAO Renderer Test
// Runs the Vulkan deferred renderer WITHOUT Godot.
// Builds a simple scene, renders, saves to PNG.
//
// Usage: ./renderer_test [output.png]

// stb_image_write — implementation already in model_gltf.cpp, just include header
// But we need our own for the standalone test
#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "render/offscreen/offscreen_renderer.hpp"
#include "scene/scene.hpp"
#include "scene/actor/actor.hpp"
#include "render/components/mesh_component.hpp"
#include "render/components/material_component.hpp"
#include "render/components/light_component.hpp"
#include "render/passes/deferred_renderer.hpp"
#include "render/camera/camera.hpp"

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

// Load a GLTF/GLB model into a scene with ground plane and lighting
std::unique_ptr<Scene> buildGLTFScene(const std::string& modelPath) {
    auto scene = std::make_unique<Scene>("GLTF Scene");

    // Load the model
    auto model = std::make_shared<Model>();
    if (!model->loadFromGLTF(modelPath)) {
        std::cerr << "Failed to load: " << modelPath << std::endl;
        return scene;
    }
    // Compute bounding box for auto-framing
    glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
    for (const auto& v : model->vertices) {
        bmin = glm::min(bmin, v.position);
        bmax = glm::max(bmax, v.position);
    }
    glm::vec3 center = (bmin + bmax) * 0.5f;
    float extent = glm::length(bmax - bmin);
    std::cout << "Loaded GLTF: " << model->vertices.size() << " vertices, "
              << model->indices.size() / 3 << " triangles"
              << " | bbox: (" << bmin.x << "," << bmin.y << "," << bmin.z
              << ") - (" << bmax.x << "," << bmax.y << "," << bmax.z
              << ") extent=" << extent << std::endl;

    // Create actor with the loaded model — raise it above ground
    auto actor = scene->createActor("Model");
    // FBX model: rotate to stand upright, then lift so feet touch ground
    actor->getTransform()->setRotation(glm::vec3(90.0f, 180.0f, 0.0f));  // face forward
    actor->getTransform()->setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
    auto meshComp = actor->addComponent<MeshComponent>();
    meshComp->setModel(model);
    meshComp->setVisible(true);
    auto matComp = actor->addComponent<MaterialComponent>();
    matComp->getMaterial().baseColor = glm::vec3(0.82f, 0.68f, 0.55f);
    matComp->getMaterial().roughness = 0.85f;   // matte — skin/fabric
    matComp->getMaterial().metallic = 0.0f;

    // Ground plane — scaled for the model
    float gs = extent * 2.0f;
    addWallQuad(scene.get(), "Ground",
        glm::vec3(-gs, -30, -gs), glm::vec3(gs, -30, -gs),
        glm::vec3(gs, -30, gs), glm::vec3(-gs, -30, gs),
        glm::vec3(0, 1, 0), glm::vec3(0.35f, 0.35f, 0.38f));

    // 3-point studio lighting — all BEHIND the camera so they're not visible
    // Camera is at center + (0.3, 0.1, 0.6) * extent, so lights go further out

    // Key light — upper-right, BEHIND camera
    float lsz = extent * 0.3f;
    addWallQuad(scene.get(), "SkyLight",
        glm::vec3(center.x + extent*0.5f, center.y + extent*0.2f, center.z + extent*0.8f),
        glm::vec3(center.x + extent*0.5f, center.y + extent*0.2f, center.z + extent*0.8f + lsz),
        glm::vec3(center.x + extent*0.5f, center.y + extent*0.5f, center.z + extent*0.8f + lsz),
        glm::vec3(center.x + extent*0.5f, center.y + extent*0.5f, center.z + extent*0.8f),
        glm::vec3(-1, 0, 0), glm::vec3(1.0f));

    // Fill light — left side, slightly behind camera
    addWallQuad(scene.get(), "FillLight",
        glm::vec3(center.x - extent*0.6f, center.y - lsz, center.z + extent*0.5f),
        glm::vec3(center.x - extent*0.6f, center.y - lsz, center.z + extent*0.5f + lsz*2),
        glm::vec3(center.x - extent*0.6f, center.y + lsz*2, center.z + extent*0.5f + lsz*2),
        glm::vec3(center.x - extent*0.6f, center.y + lsz*2, center.z + extent*0.5f),
        glm::vec3(1, 0, 0), glm::vec3(0.7f, 0.75f, 0.85f));

    // Top light — directly above model
    addWallQuad(scene.get(), "TopLight",
        glm::vec3(center.x - lsz, center.y + extent*0.5f, center.z - lsz),
        glm::vec3(center.x + lsz, center.y + extent*0.5f, center.z - lsz),
        glm::vec3(center.x + lsz, center.y + extent*0.5f, center.z + lsz),
        glm::vec3(center.x - lsz, center.y + extent*0.5f, center.z + lsz),
        glm::vec3(0, -1, 0), glm::vec3(0.9f));

    // Point light for the scene
    auto light = scene->createActorWithComponents("SunLight", PrimitiveType::DirectionalLight);
    if (light) {
        light->getTransform()->setPosition(glm::vec3(10, 20, 10));
        auto lc = light->getComponent<LightComponent>();
        if (lc) {
            lc->setDirection(glm::normalize(glm::vec3(-0.3f, -0.8f, -0.4f)));
            lc->setColor(glm::vec3(1.0f, 0.95f, 0.9f));
            lc->setIntensity(5.0f);
        }
    }

    return scene;
}

// Cornell box with a GLTF model inside
std::unique_ptr<Scene> buildCornellWithModel(const std::string& modelPath) {
    auto scene = std::make_unique<Scene>("Cornell Box + Model");

    const float W = 5.0f, H = 5.0f, D = 5.0f;
    const glm::vec3 white(0.73f, 0.73f, 0.73f);
    const glm::vec3 red(0.65f, 0.05f, 0.05f);
    const glm::vec3 green(0.12f, 0.45f, 0.15f);

    // Room corners
    glm::vec3 LBB(-W,-H,-D), RBB(W,-H,-D), LTB(-W,H,-D), RTB(W,H,-D);
    glm::vec3 LBF(-W,-H,D), RBF(W,-H,D), LTF(-W,H,D), RTF(W,H,D);

    // 5 walls
    addWallQuad(scene.get(), "BackWall",  LBB, RBB, RTB, LTB, glm::vec3(0,0,1), white);
    addWallQuad(scene.get(), "LeftWall",  LBB, LTB, LTF, LBF, glm::vec3(1,0,0), red);
    addWallQuad(scene.get(), "RightWall", RBB, RBF, RTF, RTB, glm::vec3(-1,0,0), green);
    addWallQuad(scene.get(), "Floor",     LBB, LBF, RBF, RBB, glm::vec3(0,1,0), white);
    addWallQuad(scene.get(), "Ceiling",   LTB, RTB, RTF, LTF, glm::vec3(0,-1,0), white);

    // Load the model (auto-detect format, skip if empty path)
    auto model = std::make_shared<Model>();
    bool loaded = false;
    if (!modelPath.empty()) {
        auto dot = modelPath.find_last_of('.');
        std::string ext = (dot != std::string::npos) ? modelPath.substr(dot + 1) : "";
        if (ext == "obj") loaded = model->loadFromOBJ(modelPath);
        else loaded = model->loadFromGLTF(modelPath);
    }
    if (loaded) {
        // Compute model bounds
        glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
        for (const auto& v : model->vertices) {
            bmin = glm::min(bmin, v.position);
            bmax = glm::max(bmax, v.position);
        }
        glm::vec3 extent = bmax - bmin;
        std::cout << "Model bounds: (" << bmin.x << "," << bmin.y << "," << bmin.z
                  << ") - (" << bmax.x << "," << bmax.y << "," << bmax.z
                  << ") extent=(" << extent.x << "," << extent.y << "," << extent.z << ")" << std::endl;

        // Auto-detect up axis: largest extent is the height
        bool isYUp = (extent.y >= extent.z);
        float modelHeight = isYUp ? extent.y : extent.z;
        float modelBase = isYUp ? bmin.y : bmin.z;

        // Scale to fit inside the Cornell box (80% of box height)
        float scale = (H * 1.6f) / modelHeight;

        auto actor = scene->createActor("Woman");
        if (isYUp) {
            // Model already Y-up (standard GLTF) — face toward camera (+Z)
            actor->getTransform()->setRotation(glm::quat(glm::radians(glm::vec3(0.0f, 180.0f, 0.0f))));
        } else {
            // Model is Z-up (FBX origin) — rotate to Y-up, face camera
            actor->getTransform()->setRotation(glm::quat(glm::radians(glm::vec3(-90.0f, 0.0f, 0.0f))));
        }
        actor->getTransform()->setScale(glm::vec3(scale));

        // Position: center X/Z in box, feet on floor (y = -H)
        glm::vec3 modelCenter = (bmin + bmax) * 0.5f;
        float feetOffset = isYUp ? -modelBase * scale : 0.0f;
        actor->getTransform()->setPosition(glm::vec3(
            -modelCenter.x * scale,
            -H + feetOffset,
            isYUp ? -modelCenter.z * scale : 0.0f
        ));

        auto meshComp = actor->addComponent<MeshComponent>();
        meshComp->setModel(model);
        meshComp->setVisible(true);
        auto matComp = actor->addComponent<MaterialComponent>();
        matComp->getMaterial().baseColor = glm::vec3(0.82f, 0.68f, 0.55f);
        matComp->getMaterial().roughness = 0.85f;
        matComp->getMaterial().metallic = 0.0f;

        glm::mat4 wm = actor->getTransform()->getWorldMatrix();
        std::cout << "Model: " << (isYUp ? "Y-up" : "Z-up")
                  << " height=" << modelHeight << " scale=" << scale << std::endl;
        std::cout << "WorldMatrix row0: " << wm[0][0] << " " << wm[1][0] << " " << wm[2][0] << " " << wm[3][0] << std::endl;
        std::cout << "WorldMatrix row1: " << wm[0][1] << " " << wm[1][1] << " " << wm[2][1] << " " << wm[3][1] << std::endl;
        std::cout << "WorldMatrix row2: " << wm[0][2] << " " << wm[1][2] << " " << wm[2][2] << " " << wm[3][2] << std::endl;
        std::cout << "WorldMatrix row3: " << wm[0][3] << " " << wm[1][3] << " " << wm[2][3] << " " << wm[3][3] << std::endl;
        // Test: transform model corners through world matrix
        glm::vec4 testMin = wm * glm::vec4(bmin, 1.0f);
        glm::vec4 testMax = wm * glm::vec4(bmax, 1.0f);
        std::cout << "Transformed min: " << testMin.x << "," << testMin.y << "," << testMin.z << std::endl;
        std::cout << "Transformed max: " << testMax.x << "," << testMax.y << "," << testMax.z << std::endl;
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

    // 2. Build scene — load GLTF model or fall back to Cornell box
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
    // Classic Cornell box view — slightly above center, looking in
    camera.setPosition(glm::vec3(0.0f, 0.0f, 13.0f));
    camera.setFov(38.0f);
    camera.setRotation(0.0f, -90.0f);

    // 4. Path traced mode — full RT, no rasterization
    renderer.setRenderMode(RenderMode::PathTraced);

    // 5. Render — accumulate multiple frames for convergence
    std::cout << "\n--- Path Tracing ---" << std::endl;
    auto start = std::chrono::high_resolution_clock::now();

    int numFrames = 1024;
    for (int i = 0; i < numFrames + 3; i++) {
        renderer.render();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Path trace time: " << ms << " ms" << std::endl;


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
