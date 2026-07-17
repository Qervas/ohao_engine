#pragma once

// C1 ML data factory: sample θ → FIT images (+ optional domain-rand / exposure jitter).

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

/// Write N (θ, FIT image) pairs under outDir/dataset. Returns 0 on success.
inline int runExportDataset(const FitConfig& cfg, InverseScene& inv, RenderSession& session,
                            bool studio, const std::filesystem::path& outDir) {
    const auto dataDir = outDir / "dataset";
    std::filesystem::create_directories(dataDir);

    // Param names match ParamSpace layout (for trainer column weighting).
    std::vector<std::string> dimNames;
    if (inv.mapGround) {
        for (int t = 0; t < 4; ++t) {
            dimNames.push_back("tile" + std::to_string(t) + ".R");
            dimNames.push_back("tile" + std::to_string(t) + ".G");
            dimNames.push_back("tile" + std::to_string(t) + ".B");
        }
        dimNames.push_back("primary.rough");
        dimNames.push_back("primary.metal");
    } else {
        dimNames = {"primary.R", "primary.G", "primary.B", "primary.rough", "primary.metal"};
    }
    if (inv.fitPedestal && inv.pedestalMat) {
        dimNames.push_back("pedestal.R");
        dimNames.push_back("pedestal.G");
        dimNames.push_back("pedestal.B");
    }
    if (inv.fitKeyLight && inv.keyLight) dimNames.push_back("key.I_scale");
    if (inv.fitFillLight && inv.fillLight) dimNames.push_back("fill.I_scale");
    if (inv.fitRimLight && inv.rimLight) dimNames.push_back("rim.I_scale");
    if (inv.fitEnvScale) dimNames.push_back("env.scale");
    if (inv.fitExposure) dimNames.push_back("exposure");

    {
        std::ofstream cfgOut(dataDir / "config.json");
        cfgOut << "{\n"
               << "  \"format\": \"ohao_inverse_c1\",\n"
               << "  \"version\": 2,\n"
               << "  \"dims\": " << inv.thetaDims() << ",\n"
               << "  \"preset\": \"" << cfg.preset << "\",\n"
               << "  \"scene\": \"" << cfg.scene << "\",\n"
               << "  \"map_ground\": " << (inv.mapGround ? "true" : "false") << ",\n"
               << "  \"domain_rand\": " << (cfg.domainRand ? "true" : "false") << ",\n"
               << "  \"exposure_jitter\": " << (cfg.exportExposureJitter ? "true" : "false")
               << ",\n"
               << "  \"export_view_mode\": " << cfg.exportViewMode << ",\n"
               << "  \"fit\": {\"width\": " << cfg.fit.width << ", \"height\": " << cfg.fit.height
               << ", \"spp\": " << cfg.fit.spp << "},\n"
               << "  \"names\": [";
        for (size_t i = 0; i < dimNames.size(); ++i) {
            if (i) cfgOut << ", ";
            cfgOut << "\"" << dimNames[i] << "\"";
        }
        cfgOut << "]\n}\n";
    }

    std::ofstream meta(dataDir / "meta.jsonl");
    meta << "{\"type\":\"header\",\"dims\":" << inv.thetaDims()
         << ",\"fit\":[" << cfg.fit.width << "," << cfg.fit.height << "," << cfg.fit.spp
         << "],\"scene\":\"" << cfg.scene << "\",\"preset\":\"" << cfg.preset
         << "\",\"map_ground\":" << (inv.mapGround ? "true" : "false")
         << ",\"domain_rand\":" << (cfg.domainRand ? "true" : "false")
         << ",\"version\":2}\n";
    std::cout << "Exporting " << cfg.exportDataset << " (θ, image) pairs to " << dataDir
              << " (C1 factory"
              << (cfg.domainRand ? ", domain-rand" : "")
              << (cfg.exportExposureJitter ? ", exp-jitter" : "") << ")...\n";

    int written = 0;
    for (int i = 0; i < cfg.exportDataset; ++i) {
        uint32_t rng = cfg.seed + static_cast<uint32_t>(i) * 2654435761u;
        const auto th = inv.sampleRandomTheta(rng);
        inv.applyTheta(th);

        int viewIdx = 0;
        std::string viewName = "front";
        if (cfg.domainRand && studio) {
            viewIdx = inv.applyDomainRandomization(rng);
            viewName = inv.views.empty() ? "orbit" : inv.views[0].name;
            session.bound = false; // transforms changed — full rebind
        } else if (cfg.exportViewMode == 1 && !inv.views.empty()) {
            viewIdx = static_cast<int>(rng % static_cast<uint32_t>(inv.views.size()));
            viewName = inv.views[static_cast<size_t>(viewIdx)].name;
        }

        auto emitSample = [&](int vIdx, const std::string& vName, int sampleId) {
            ImageRGBA8 img =
                session.render(vIdx, cfg.fit, cfg.seed + static_cast<uint32_t>(sampleId),
                               DenoiseMode::None);
            float expGain = 1.0f;
            if (cfg.exportExposureJitter) {
                // Mild LDR exposure domain gap (C2/L4-lite)
                uint32_t er = cfg.seed + static_cast<uint32_t>(sampleId) * 97u + 13u;
                er = er * 1664525u + 1013904223u;
                const float u = static_cast<float>(er >> 8) / static_cast<float>(1u << 24);
                expGain = 0.75f + u * 0.55f; // [0.75, 1.30]
                img = applyExposure(img, expGain);
            }
            char name[40];
            std::snprintf(name, sizeof(name), "%05d.png", sampleId);
            savePNG(img, dataDir / name);
            meta << "{\"i\":" << sampleId << ",\"file\":\"" << name << "\",\"preset\":\""
                 << cfg.preset << "\",\"scene\":\"" << cfg.scene << "\",\"view\":\"" << vName
                 << "\",\"domain_rand\":" << (cfg.domainRand ? "true" : "false")
                 << ",\"exposure\":" << expGain << ",\"theta\":[";
            for (size_t k = 0; k < th.size(); ++k) {
                if (k) meta << ",";
                meta << th[k];
            }
            meta << "]}\n";
        };

        if (cfg.exportViewMode == 2 && !inv.views.empty() && !cfg.domainRand) {
            for (int v = 0; v < static_cast<int>(inv.views.size()); ++v) {
                emitSample(v, inv.views[static_cast<size_t>(v)].name, written);
                ++written;
            }
        } else {
            emitSample(viewIdx, viewName, written);
            ++written;
        }

        if (cfg.domainRand && studio) inv.resetDomainDefaults();

        if ((i + 1) % 10 == 0 || i + 1 == cfg.exportDataset) {
            std::cout << "  " << (i + 1) << "/" << cfg.exportDataset << " (files=" << written
                      << ")\n";
        }
    }
    meta.close();
    std::cout << "Dataset export done → " << dataDir << " (" << written << " images)\n"
              << "  Train: python3 tools/inverse_c1/train.py --data " << dataDir << "\n";
    return 0;
}

} // namespace ohao::inverse
