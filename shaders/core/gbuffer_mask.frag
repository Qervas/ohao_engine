#version 450
// G-Buffer fragment shader, alpha-tested materials (glTF MASK, gpu::shared::kAlphaMask): the
// body of core/gbuffer.frag with a discard below the material's cutoff, so no forced early
// depth test. GBufferPass draws the kDrawMask draws with it after the late phase.
#define OHAO_GBUFFER_MASK 1
#include "includes/scene/gbuffer_body.glsl"
