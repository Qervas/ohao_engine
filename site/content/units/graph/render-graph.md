---
module: graph
id: render-graph
title: RenderGraph
---

## What

RenderGraph declares passes, resources, barriers; compiles execution order.

## How

Deferred uses graph for CSM→GBuffer→SSAO→Lighting barriers while passes still own VkRenderPass objects.

## Why

Explicit barriers beat tribal knowledge about image layouts.

## Contracts

- Sources of truth: ohao/render/graph/render_graph.hpp
- Sources of truth: ohao/render/graph/render_graph.cpp
- Sources of truth: ohao/render/graph/render_pass.hpp
- Sources of truth: ohao/render/graph/resource_handle.hpp

## Notes

Deferred uses graph for CSM→GBuffer→SSAO→Lighting barriers while passes own VkRenderPass objects.

## Notes

Source map:
- `ohao/render/graph/render_graph.hpp`
- `ohao/render/graph/render_graph.cpp`
- `ohao/render/graph/render_pass.hpp`
- `ohao/render/graph/resource_handle.hpp`
