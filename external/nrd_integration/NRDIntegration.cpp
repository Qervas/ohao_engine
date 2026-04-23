// Vendored from NVIDIA RayTracingDenoiser v4.17.2
// https://github.com/NVIDIAGameWorks/RayTracingDenoiser
// License: NVIDIA RTX SDKs License (carries upstream terms)
//
// Single-TU wrapper for NRDIntegration.hpp.
//
// NRDIntegration.hpp is a header-only .hpp whose body is pure implementation
// (no #ifdef NRD_INTEGRATION_IMPL gating macro). It must therefore be included
// from exactly one translation unit in the consuming project. That TU is
// this file.
//
// Prerequisites the .hpp's top-level #error guards enforce:
//   - NRD.h          (defines NRD_VERSION_MAJOR)
//   - NRI.h          (defines NRI_VERSION)
//   - NRIHelper.h    (defines NRI_HELPER_H)
// These must be included before NRDIntegration.h (itself pulled in by
// NRDIntegration.hpp). We include NRDIntegration.hpp last.
//
// Sub-plan 4.C T3a — existence of this .cpp flips CMake's .cpp-presence
// glob in external/cmake/nrd.cmake from "headers-only" to "build the
// NRDIntegration static lib," which ohao_renderer then links via its
// existing if(TARGET NRDIntegration) gate.

// cstring/cstdio provide memcpy/snprintf — used by NRDIntegration.hpp but
// not included transitively by any of its other deps.
#include <cstdio>
#include <cstring>

#include "NRD.h"
#include "NRI.h"
#include "Extensions/NRIHelper.h"
#include "Extensions/NRIDeviceCreation.h"  // nriDestroyDevice declaration

#include "NRDIntegration.hpp"
