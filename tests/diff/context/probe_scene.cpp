// The probe scene. See probe_scene.hpp for why this is its own translation
// unit; the definition below is gpu_probe_context.cpp's, unchanged.
#include "context/probe_scene.hpp"

namespace ohao::diff::probe_scene {
void buildAxisAlignedBoxGeometry(float halfExtent, std::vector<float>& positions,
                                 std::vector<uint32_t>& indices) {
    positions.clear();
    indices.clear();
    positions.reserve(24u * 3u);
    indices.reserve(12u * 3u);

    const float e = halfExtent;
    for (uint32_t k = 0; k < 3u; ++k) {
        for (int signIndex = 0; signIndex < 2; ++signIndex) {
            const float s = (signIndex == 0) ? 1.0f : -1.0f;
            uint32_t u = (k + 1u) % 3u;
            uint32_t v = (k + 2u) % 3u;
            if (s < 0.0f) {
                const uint32_t swap = u;
                u = v;
                v = swap;
            }

            const float du[4] = {-e, e, e, -e};
            const float dv[4] = {-e, -e, e, e};
            const uint32_t base = static_cast<uint32_t>(positions.size() / 3u);
            for (int corner = 0; corner < 4; ++corner) {
                float pos[3] = {0.0f, 0.0f, 0.0f};
                pos[k] = s * e;
                pos[u] = du[corner];
                pos[v] = dv[corner];
                positions.push_back(pos[0]);
                positions.push_back(pos[1]);
                positions.push_back(pos[2]);
            }
            const uint32_t tris[6] = {base + 0u, base + 1u, base + 2u,
                                      base + 0u, base + 2u, base + 3u};
            indices.insert(indices.end(), std::begin(tris), std::end(tris));
        }
    }
}

}  // namespace ohao::diff::probe_scene
