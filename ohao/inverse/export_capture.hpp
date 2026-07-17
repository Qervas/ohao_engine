#pragma once

// Lab L1: export multi-view capture bundle (train/holdout + relight GT).

#include "inverse/fit_config.hpp"
#include "inverse/image_loss.hpp"
#include "inverse/io.hpp"
#include "inverse/render_session.hpp"
#include "inverse/scene_builder.hpp"

#include "render/rt/denoise/denoise_types.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace ohao::inverse {

/// Write constant RGB map (L3 placeholder: scalar materials as textures).
inline bool writeConstantRGBMap(const std::filesystem::path& path, float r, float g, float b,
                                int res = 64) {
    ImageRGBA8 img;
    img.width = static_cast<uint32_t>(res);
    img.height = static_cast<uint32_t>(res);
    img.rgba.resize(static_cast<size_t>(res) * res * 4);
    const uint8_t R = static_cast<uint8_t>(std::clamp(r, 0.f, 1.f) * 255.f + 0.5f);
    const uint8_t G = static_cast<uint8_t>(std::clamp(g, 0.f, 1.f) * 255.f + 0.5f);
    const uint8_t B = static_cast<uint8_t>(std::clamp(b, 0.f, 1.f) * 255.f + 0.5f);
    for (int i = 0; i < res * res; ++i) {
        img.rgba[static_cast<size_t>(i) * 4 + 0] = R;
        img.rgba[static_cast<size_t>(i) * 4 + 1] = G;
        img.rgba[static_cast<size_t>(i) * 4 + 2] = B;
        img.rgba[static_cast<size_t>(i) * 4 + 3] = 255;
    }
    return savePNG(img, path);
}

/// ORM-style: R unused=1, G=roughness, B=metallic.
inline bool writeConstantORMMap(const std::filesystem::path& path, float rough, float metal,
                                int res = 64) {
    return writeConstantRGBMap(path, 1.f, rough, metal, res);
}

/// Write ground albedo (+ optional ORM) maps from θ (or truthTiles if th empty).
inline void writeGroundMapsFromTheta(const InverseScene& inv, const std::vector<double>& th,
                                     const std::filesystem::path& matDir,
                                     const std::string& tag) {
    std::filesystem::create_directories(matDir);
    if (inv.mapGround && inv.tileCount() > 0) {
        const int N = inv.mapRes;
        ImageRGBA8 alb;
        alb.width = static_cast<uint32_t>(N);
        alb.height = static_cast<uint32_t>(N);
        alb.rgba.assign(static_cast<size_t>(N) * N * 4, 255);
        const size_t nT = static_cast<size_t>(inv.tileCount());
        const bool useTh = th.size() >= nT * 3 + 2;
        for (int k = 0; k < N * N; ++k) {
            float r, g, b;
            if (useTh) {
                const size_t o = static_cast<size_t>(k) * 3;
                r = static_cast<float>(th[o]);
                g = static_cast<float>(th[o + 1]);
                b = static_cast<float>(th[o + 2]);
            } else {
                const auto& c = inv.truthTiles[static_cast<size_t>(k)];
                r = c.r;
                g = c.g;
                b = c.b;
            }
            const size_t o = static_cast<size_t>(k) * 4;
            alb.rgba[o + 0] = static_cast<uint8_t>(std::clamp(r, 0.f, 1.f) * 255.f + 0.5f);
            alb.rgba[o + 1] = static_cast<uint8_t>(std::clamp(g, 0.f, 1.f) * 255.f + 0.5f);
            alb.rgba[o + 2] = static_cast<uint8_t>(std::clamp(b, 0.f, 1.f) * 255.f + 0.5f);
        }
        savePNG(alb, matDir / ("ground_albedo_" + tag + ".png"));
        float rough = inv.truthPrimary.roughness, metal = inv.truthPrimary.metallic;
        if (useTh) {
            rough = static_cast<float>(th[nT * 3]);
            metal = static_cast<float>(th[nT * 3 + 1]);
        }
        writeConstantORMMap(matDir / ("ground_orm_" + tag + ".png"), rough, metal, N);
    } else {
        float r = inv.truthPrimary.albedo.r, g = inv.truthPrimary.albedo.g,
              b = inv.truthPrimary.albedo.b;
        float rough = inv.truthPrimary.roughness, metal = inv.truthPrimary.metallic;
        if (th.size() >= 5) {
            r = static_cast<float>(th[0]);
            g = static_cast<float>(th[1]);
            b = static_cast<float>(th[2]);
            rough = static_cast<float>(th[3]);
            metal = static_cast<float>(th[4]);
        }
        writeConstantRGBMap(matDir / ("ground_albedo_" + tag + ".png"), r, g, b);
        writeConstantORMMap(matDir / ("ground_orm_" + tag + ".png"), rough, metal);
    }
}

/// Export lab capture under outDir/capture/. Returns 0 on success.
inline int runExportCapture(const FitConfig& cfg, InverseScene& inv, RenderSession& session,
                            bool studio, const std::filesystem::path& outDir) {
    const auto capRoot = outDir / "capture";
    const auto imgDir = capRoot / "images";
    const auto relightDir = capRoot / "relight";
    const auto matDir = capRoot / "materials";
    std::filesystem::create_directories(imgDir);
    std::filesystem::create_directories(relightDir);
    std::filesystem::create_directories(matDir);

    const int nAll = static_cast<int>(inv.views.size());
    if (nAll < 1) {
        std::cerr << "FATAL: no cameras in scene\n";
        return 1;
    }
    // Use up to cfg.numViews; last view is holdout if >= 2 views.
    const int nUse = std::min(cfg.numViews, nAll);
    const int nHoldout = (nUse >= 2) ? 1 : 0;
    const int nTrain = nUse - nHoldout;

    inv.applyTruth();
    const auto truth = inv.truthTheta();

    // Material maps (spatial tiles → PNG for L3 contract).
    if (inv.mapGround && !inv.truthTiles.empty()) {
        const int N = inv.mapRes;
        ImageRGBA8 alb;
        alb.width = static_cast<uint32_t>(N);
        alb.height = static_cast<uint32_t>(N);
        alb.rgba.resize(static_cast<size_t>(N) * N * 4);
        for (int iz = 0; iz < N; ++iz) {
            for (int ix = 0; ix < N; ++ix) {
                const int k = iz * N + ix;
                const auto& c = inv.truthTiles[static_cast<size_t>(k)];
                const size_t o = static_cast<size_t>(k) * 4;
                alb.rgba[o + 0] = static_cast<uint8_t>(std::clamp(c.r, 0.f, 1.f) * 255.f + 0.5f);
                alb.rgba[o + 1] = static_cast<uint8_t>(std::clamp(c.g, 0.f, 1.f) * 255.f + 0.5f);
                alb.rgba[o + 2] = static_cast<uint8_t>(std::clamp(c.b, 0.f, 1.f) * 255.f + 0.5f);
                alb.rgba[o + 3] = 255;
            }
        }
        savePNG(alb, matDir / "ground_albedo.png");
        // init maps for wrong-start evidence
        ImageRGBA8 initAlb = alb;
        for (int k = 0; k < N * N; ++k) {
            const auto& c = inv.initTiles[static_cast<size_t>(k)];
            const size_t o = static_cast<size_t>(k) * 4;
            initAlb.rgba[o + 0] = static_cast<uint8_t>(std::clamp(c.r, 0.f, 1.f) * 255.f + 0.5f);
            initAlb.rgba[o + 1] = static_cast<uint8_t>(std::clamp(c.g, 0.f, 1.f) * 255.f + 0.5f);
            initAlb.rgba[o + 2] = static_cast<uint8_t>(std::clamp(c.b, 0.f, 1.f) * 255.f + 0.5f);
        }
        savePNG(initAlb, matDir / "ground_albedo_init.png");
    } else {
        writeConstantRGBMap(matDir / "ground_albedo.png", inv.truthPrimary.albedo.r,
                            inv.truthPrimary.albedo.g, inv.truthPrimary.albedo.b);
        writeConstantRGBMap(matDir / "ground_albedo_init.png", inv.initPrimary.albedo.r,
                            inv.initPrimary.albedo.g, inv.initPrimary.albedo.b);
    }
    writeConstantORMMap(matDir / "ground_orm.png", inv.truthPrimary.roughness,
                        inv.truthPrimary.metallic);
    writeConstantORMMap(matDir / "ground_orm_init.png", inv.initPrimary.roughness,
                        inv.initPrimary.metallic);
    writeConstantRGBMap(matDir / "pedestal_albedo.png", inv.truthPedestal.albedo.r,
                        inv.truthPedestal.albedo.g, inv.truthPedestal.albedo.b);

    // Cameras + train/holdout images (SHOW budget, denoise for clean lab GT).
    std::ofstream camLog(capRoot / "cameras.jsonl");
    for (int v = 0; v < nUse; ++v) {
        const auto& cv = inv.views[static_cast<size_t>(v)];
        const bool holdout = (v >= nTrain);
        char name[64];
        if (holdout) {
            std::snprintf(name, sizeof(name), "holdout_%03d.png", v - nTrain);
        } else {
            std::snprintf(name, sizeof(name), "train_%03d.png", v);
        }
        // Lab capture GT: no denoise (match FD train path). Higher spp compensates.
        ImageRGBA8 img = session.render(v, cfg.show, cfg.seed, DenoiseMode::None);
        savePNG(img, imgDir / name);

        camLog << "{\"index\":" << v << ",\"name\":\"" << cv.name << "\",\"file\":\"" << name
               << "\",\"split\":\"" << (holdout ? "holdout" : "train") << "\",\"position\":["
               << cv.position.x << "," << cv.position.y << "," << cv.position.z
               << "],\"pitch_deg\":" << cv.pitchDeg << ",\"yaw_deg\":" << cv.yawDeg
               << ",\"fov_deg\":40.0}\n";
    }
    camLog.close();

    // Relight GT under alternate env (same θ, same cameras).
    if (studio && !inv.relightEnvPath.empty() && std::filesystem::exists(inv.relightEnvPath)) {
        session.rebindEnv(inv.relightEnvPath);
        inv.applyTruth();
        for (int v = 0; v < nUse; ++v) {
            const bool holdout = (v >= nTrain);
            char name[64];
            if (holdout) {
                std::snprintf(name, sizeof(name), "holdout_%03d.png", v - nTrain);
            } else {
                std::snprintf(name, sizeof(name), "train_%03d.png", v);
            }
            ImageRGBA8 img = session.render(v, cfg.show, cfg.seed + 17u, DenoiseMode::None);
            savePNG(img, relightDir / name);
        }
        // Restore primary env for caller
        session.rebindEnv(inv.envPath);
        inv.applyTruth();
    }

    // theta_gt.json
    {
        std::ofstream th(capRoot / "theta_gt.json");
        th << "{\n  \"theta\": [";
        for (size_t i = 0; i < truth.size(); ++i) {
            if (i) th << ", ";
            th << truth[i];
        }
        th << "],\n  \"names\": [";
        // Names mirror buildParamSpace
        std::vector<std::string> names;
        if (inv.mapGround) {
            for (int ti = 0; ti < inv.tileCount(); ++ti) {
                names.push_back("tile" + std::to_string(ti) + ".R");
                names.push_back("tile" + std::to_string(ti) + ".G");
                names.push_back("tile" + std::to_string(ti) + ".B");
            }
            names.push_back("primary.rough");
            names.push_back("primary.metal");
        } else {
            names = {"primary.R", "primary.G", "primary.B", "primary.rough", "primary.metal"};
        }
        if (inv.fitPedestal && inv.pedestalMat) {
            names.push_back("pedestal.R");
            names.push_back("pedestal.G");
            names.push_back("pedestal.B");
        }
        if (inv.fitKeyLight && inv.keyLight) names.push_back("key.I_scale");
        if (inv.fitFillLight && inv.fillLight) names.push_back("fill.I_scale");
        if (inv.fitRimLight && inv.rimLight) names.push_back("rim.I_scale");
        if (inv.fitEnvScale) names.push_back("env.scale");
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) th << ", ";
            th << "\"" << names[i] << "\"";
        }
        th << "]\n}\n";
    }

    // capture.json manifest
    {
        std::ofstream man(capRoot / "capture.json");
        man << "{\n"
            << "  \"format\": \"ohao_inverse_lab_capture\",\n"
            << "  \"version\": 1,\n"
            << "  \"preset\": \"" << cfg.preset << "\",\n"
            << "  \"scene\": \"" << cfg.scene << "\",\n"
            << "  \"model_path\": \"" << cfg.modelPath << "\",\n"
            << "  \"env_path\": \"" << cfg.envPath << "\",\n"
            << "  \"relight_env_path\": \"" << cfg.relightEnvPath << "\",\n"
            << "  \"map_ground\": " << (inv.mapGround ? "true" : "false") << ",\n"
            << "  \"map_res\": " << inv.mapRes << ",\n"
            << "  \"theta_dims\": " << truth.size() << ",\n"
            << "  \"n_train\": " << nTrain << ",\n"
            << "  \"n_holdout\": " << nHoldout << ",\n"
            << "  \"show\": {\"width\": " << cfg.show.width << ", \"height\": " << cfg.show.height
            << ", \"spp\": " << cfg.show.spp << "},\n"
            << "  \"fit\": {\"width\": " << cfg.fit.width << ", \"height\": " << cfg.fit.height
            << ", \"spp\": " << cfg.fit.spp << "},\n"
            << "  \"notes\": \"Train views for fit; holdout for novel-view metric; relight/ for "
               "env transfer eval.\"\n"
            << "}\n";
    }

    std::cout << "Lab capture exported → " << capRoot << "\n"
              << "  train=" << nTrain << " holdout=" << nHoldout << " dims=" << truth.size()
              << "\n"
              << "  Fit: ./build/inverse_fit --lab-bundle " << capRoot
              << " --preset " << cfg.preset << " --quality " << cfg.quality.name << "\n";
    return 0;
}

} // namespace ohao::inverse
