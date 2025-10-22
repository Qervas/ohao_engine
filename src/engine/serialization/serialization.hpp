#pragma once

// Deprecated aggregation header. Use map_io.hpp directly.
#include "map_io.hpp"

namespace ohao {
    namespace serialization {
        // Backward-compat factory returning MapIO
        inline MapIO createSceneSerializer(Scene* scene) { return MapIO(scene); }
    }
} 