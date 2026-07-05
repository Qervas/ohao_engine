# NRI — NVIDIA Render Interface (required by NRDIntegration)
# Thin D3D11/D3D12/Vulkan/Metal abstraction layer. For OHAO (Linux/Vulkan only)
# we trim the build surface to Vulkan.
# https://github.com/NVIDIA-RTX/NRI
#
# Version pairing is non-trivial: NRDIntegration.hpp has
#   static_assert(NRI_VERSION >= 178, ...)
# and NRD v4.17.2 ships with its own upstream-pinned NRI fetch at tag `v178`.
# Rather than duplicate that pin here and risk drift, we flip NRD's built-in
# option `NRD_NRI=ON` so NRD itself fetches the upstream-paired NRI archive.
# This module's job is only to pre-set NRI's cache options *before* NRD's
# FetchContent_MakeAvailable() runs — that way NRI picks up OHAO's trimmed
# Vulkan-only build surface.
#
# Include this module BEFORE external/cmake/nrd.cmake so the CACHE vars
# are visible to NRI's CMake when NRD triggers the fetch.

# Trim NRI build surface — Vulkan-only on Linux. D3D11/D3D12 have no support
# on Linux anyway; Metal is Apple-only; validation layer + NVAPI/AMDAGS pull
# in Windows-only bits we don't need.
set(NRI_ENABLE_VK_SUPPORT        ON  CACHE BOOL "" FORCE)
set(NRI_ENABLE_D3D11_SUPPORT     OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_D3D12_SUPPORT     OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_METAL_SUPPORT     OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_NVAPI             OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_AMDAGS            OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_NVTX_SUPPORT      OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_NONE_SUPPORT      OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_VALIDATION_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_STATIC_LIBRARY           ON  CACHE BOOL "" FORCE)

# Delegate the actual fetch to NRD's CMakeLists — it pins NRI to the
# upstream-compatible tag (currently `v178` for NRD v4.17.2).
set(NRD_NRI                      ON  CACHE BOOL "" FORCE)
