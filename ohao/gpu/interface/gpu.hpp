#pragma once

// OHAO GPU Abstraction Layer
//
// Backend-agnostic interface for GPU resources and commands.
// Implementations: gpu/vulkan/ (current), gpu/cuda/ (future)
//
// Usage:
//   auto device = ohao::gpu::createVulkanDevice();
//   auto buffer = device->createBuffer({.size = 1024, .usage = BufferUsage::Storage});
//   device->uploadBuffer(buffer, data, dataSize);
//
//   auto cmd = device->createCommandList();
//   cmd->begin();
//   cmd->bindPipeline(pipeline);
//   cmd->traceRays(width, height, 1);
//   cmd->submitAndWait();

#include "types.hpp"
#include "device.hpp"
#include "command_list.hpp"
#include "pipeline.hpp"
