# DLSS — NVIDIA DLSS Ray Reconstruction (NGX "dlssd"), Phase 5.
#
# The vendored SDK ships a thin STATIC wrapper (libnvsdk_ngx.a) that dlopens the
# installed driver component (/usr/lib64/libnvidia-ngx.so.1) at runtime. We link
# ONLY the static wrapper — never the system .so (that would double the symbols).
# The DLSS-RR model snippet (libnvidia-ngx-dlssd.so.*) lives under lib/.../rel/;
# NGX finds it at runtime via NVSDK_NGX_FeatureCommonInfo.PathListInfo (which the
# DlssRR wrapper points at OHAO_DLSS_SNIPPET_DIR) — we also copy it next to the
# binaries as belt-and-suspenders so LD_LIBRARY_PATH is not strictly required.
#
# https://github.com/NVIDIA/DLSS  /  nvpro-samples/vk_denoise_dlssrr

# Windows: the NGX loader for the static CRT (/MT, the engine's) (nvsdk_ngx_s.lib) and the model nvngx_dlssd.dll,
# fetched into the git-ignored external/DLSS by tools/fetch_dlss.py (pinned release, hashes).
set(OHAO_DLSS_DIR         ${CMAKE_SOURCE_DIR}/external/DLSS)
set(OHAO_DLSS_INCLUDE_DIR ${OHAO_DLSS_DIR}/include)
if(WIN32)
    set(OHAO_DLSS_STATIC_LIB  ${OHAO_DLSS_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib)
    set(OHAO_DLSS_SNIPPET_DIR ${OHAO_DLSS_DIR}/lib/Windows_x86_64/rel CACHE INTERNAL "the DLSS RR model's directory (ohao/render bakes it in)")
    set(OHAO_DLSS_SNIPPET_GLOB "${OHAO_DLSS_SNIPPET_DIR}/nvngx_dlssd.dll")
else()
    set(OHAO_DLSS_STATIC_LIB  ${OHAO_DLSS_DIR}/lib/Linux_x86_64/libnvsdk_ngx.a)
    set(OHAO_DLSS_SNIPPET_DIR ${OHAO_DLSS_DIR}/lib/Linux_x86_64/rel CACHE INTERNAL "the DLSS RR model's directory (ohao/render bakes it in)")
    set(OHAO_DLSS_SNIPPET_GLOB "${OHAO_DLSS_SNIPPET_DIR}/libnvidia-ngx-dlssd.so*")
endif()

if(NOT EXISTS ${OHAO_DLSS_STATIC_LIB})
    message(WARNING "DLSS: static wrapper not found at ${OHAO_DLSS_STATIC_LIB} "
                    "— DLSS integration will be unavailable (ngx_dlss target not created)")
    return()
endif()

# Imported STATIC target so include dirs + the archive propagate to consumers.
# GLOBAL so sibling subdirectories (ohao/render, ohao/gpu) can reference it.
add_library(ngx_dlss STATIC IMPORTED GLOBAL)
set_target_properties(ngx_dlss PROPERTIES
    IMPORTED_LOCATION             ${OHAO_DLSS_STATIC_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${OHAO_DLSS_INCLUDE_DIR})

# Copy the dlssd snippet .so next to the built binaries (configure-time).
file(GLOB OHAO_DLSS_SNIPPET_SOS "${OHAO_DLSS_SNIPPET_GLOB}")
if(OHAO_DLSS_SNIPPET_SOS)
    file(COPY ${OHAO_DLSS_SNIPPET_SOS} DESTINATION ${CMAKE_BINARY_DIR})
    if(WIN32)
        file(COPY ${OHAO_DLSS_SNIPPET_SOS} DESTINATION ${CMAKE_BINARY_DIR}/Release)
    endif()
    message(STATUS "DLSS: copied dlssd snippet to ${CMAKE_BINARY_DIR}")
else()
    message(WARNING "DLSS: no dlssd snippet (.so) found under ${OHAO_DLSS_SNIPPET_DIR} "
                    "— feature creation will fail at runtime unless it is on LD_LIBRARY_PATH")
endif()

# NGX needs a writable application-data directory at runtime.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/ngx_appdata)

message(STATUS "DLSS: enabled (static wrapper ${OHAO_DLSS_STATIC_LIB})")
