// common.glsl - Common GLSL setup for OHAO Engine shaders
// GLSL include file - use with glslangValidator -I flag
//
// Part of OHAO Engine shader system
// Location: includes/common/common.glsl
//
// This file should be included first in all shaders to establish
// common version, extensions, and base macros.

#ifndef OHAO_COMMON_GLSL
#define OHAO_COMMON_GLSL

// Vulkan GLSL version is set in the shader file itself (#version 450)
// This file provides common extensions and utilities

// Common extensions (only if not already enabled by the main shader)
// GL_GOOGLE_include_directive - enables #include
// GL_EXT_nonuniform_qualifier - enables nonuniformEXT for texture arrays

// Precision qualifiers (for compatibility)
#ifdef GL_ES
precision highp float;
precision highp int;
#endif

// Include constants and math utilities
#include "includes/common/constants.glsl"
#include "includes/common/math.glsl"

#endif // OHAO_COMMON_GLSL
