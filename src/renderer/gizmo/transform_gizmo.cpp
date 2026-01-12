#include "transform_gizmo.hpp"
#include "renderer/vulkan_context.hpp"
#include "rhi/vk/ohao_vk_buffer.hpp"
#include <stdexcept>
#include <cmath>

namespace ohao {

bool TransformGizmo::initialize(VulkanContext* contextPtr) {
    context = contextPtr;

    createArrowGeometry();
    if (!createBuffers()) {
        return false;
    }

    return true;
}

void TransformGizmo::cleanup() {
    vertexBuffer.reset();
    indexBuffer.reset();
    vertices.clear();
    indices.clear();
}

void TransformGizmo::createArrowGeometry() {
    vertices.clear();
    indices.clear();

    const int segments = 12; // Number of segments for cylinder/cone
    const float PI = 3.14159265359f;

    auto createArrow = [&](const glm::vec3& dir, const glm::vec3& color, uint32_t& indexOffset, uint32_t& indexCount) {
        uint32_t startIndex = static_cast<uint32_t>(vertices.size());
        uint32_t startIndices = static_cast<uint32_t>(indices.size());

        // Determine perpendicular vectors for the cylinder
        glm::vec3 perp1, perp2;
        if (std::abs(dir.x) < 0.9f) {
            perp1 = glm::normalize(glm::cross(dir, glm::vec3(1, 0, 0)));
        } else {
            perp1 = glm::normalize(glm::cross(dir, glm::vec3(0, 1, 0)));
        }
        perp2 = glm::normalize(glm::cross(dir, perp1));

        // Shaft cylinder vertices
        for (int i = 0; i <= segments; ++i) {
            float angle = (2.0f * PI * i) / segments;
            float cosA = std::cos(angle);
            float sinA = std::sin(angle);

            glm::vec3 offset = (perp1 * cosA + perp2 * sinA) * arrowThickness;

            // Bottom ring (at origin)
            vertices.push_back({
                offset,
                color,
                glm::normalize(offset),
                {static_cast<float>(i) / segments, 0.0f}
            });

            // Top ring (at shaft end, before cone)
            glm::vec3 shaftEnd = dir * (arrowLength - coneLength);
            vertices.push_back({
                shaftEnd + offset,
                color,
                glm::normalize(offset),
                {static_cast<float>(i) / segments, 1.0f}
            });
        }

        // Shaft indices (triangle strip converted to triangles)
        uint32_t baseIdx = startIndex;
        for (int i = 0; i < segments; ++i) {
            uint32_t bl = baseIdx + i * 2;
            uint32_t br = baseIdx + i * 2 + 2;
            uint32_t tl = baseIdx + i * 2 + 1;
            uint32_t tr = baseIdx + i * 2 + 3;

            indices.push_back(bl);
            indices.push_back(br);
            indices.push_back(tl);

            indices.push_back(tl);
            indices.push_back(br);
            indices.push_back(tr);
        }

        // Cone tip
        uint32_t coneBase = static_cast<uint32_t>(vertices.size());
        glm::vec3 coneStart = dir * (arrowLength - coneLength);
        glm::vec3 coneTip = dir * arrowLength;

        // Cone base ring
        for (int i = 0; i <= segments; ++i) {
            float angle = (2.0f * PI * i) / segments;
            float cosA = std::cos(angle);
            float sinA = std::sin(angle);

            glm::vec3 offset = (perp1 * cosA + perp2 * sinA) * coneRadius;
            glm::vec3 normal = glm::normalize(offset + dir * (coneRadius / coneLength));

            vertices.push_back({
                coneStart + offset,
                color,
                normal,
                {static_cast<float>(i) / segments, 0.0f}
            });
        }

        // Cone tip vertex
        uint32_t tipIdx = static_cast<uint32_t>(vertices.size());
        vertices.push_back({
            coneTip,
            color,
            dir,
            {0.5f, 1.0f}
        });

        // Cone indices
        for (int i = 0; i < segments; ++i) {
            indices.push_back(coneBase + i);
            indices.push_back(coneBase + i + 1);
            indices.push_back(tipIdx);
        }

        // Cone base cap (optional, for solid look)
        uint32_t capCenter = static_cast<uint32_t>(vertices.size());
        vertices.push_back({
            coneStart,
            color,
            -dir,
            {0.5f, 0.5f}
        });

        for (int i = 0; i < segments; ++i) {
            indices.push_back(capCenter);
            indices.push_back(coneBase + i + 1);
            indices.push_back(coneBase + i);
        }

        indexOffset = startIndices;
        indexCount = static_cast<uint32_t>(indices.size()) - startIndices;
    };

    // X axis (Red)
    createArrow(glm::vec3(1, 0, 0), glm::vec3(1.0f, 0.2f, 0.2f), xAxisIndexOffset, xAxisIndexCount);

    // Y axis (Green)
    createArrow(glm::vec3(0, 1, 0), glm::vec3(0.2f, 1.0f, 0.2f), yAxisIndexOffset, yAxisIndexCount);

    // Z axis (Blue)
    createArrow(glm::vec3(0, 0, 1), glm::vec3(0.2f, 0.2f, 1.0f), zAxisIndexOffset, zAxisIndexCount);
}

bool TransformGizmo::createBuffers() {
    try {
        if (!vertices.empty()) {
            VkDeviceSize vertexBufferSize = sizeof(vertices[0]) * vertices.size();
            vertexBuffer = std::make_unique<OhaoVkBuffer>();
            vertexBuffer->initialize(context->getLogicalDevice());

            if (!OhaoVkBuffer::createWithStaging(
                context->getLogicalDevice(),
                context->getVkCommandPool(),
                vertices.data(),
                vertexBufferSize,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                *vertexBuffer)) {
                throw std::runtime_error("Failed to create transform gizmo vertex buffer");
            }
        }

        if (!indices.empty()) {
            VkDeviceSize indexBufferSize = sizeof(indices[0]) * indices.size();
            indexBuffer = std::make_unique<OhaoVkBuffer>();
            indexBuffer->initialize(context->getLogicalDevice());

            if (!OhaoVkBuffer::createWithStaging(
                context->getLogicalDevice(),
                context->getVkCommandPool(),
                indices.data(),
                indexBufferSize,
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                *indexBuffer)) {
                throw std::runtime_error("Failed to create transform gizmo index buffer");
            }
        }

        return true;
    }
    catch (const std::exception& e) {
        cleanup();
        return false;
    }
}

void TransformGizmo::render(VkCommandBuffer cmdBuffer, const glm::mat4& viewProj, const glm::vec3& position) {
    if (!isVisible || !vertexBuffer || !indexBuffer) return;

    VkBuffer vbuffer = vertexBuffer->getBuffer();
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmdBuffer, 0, 1, &vbuffer, offsets);
    vkCmdBindIndexBuffer(cmdBuffer, indexBuffer->getBuffer(), 0, VK_INDEX_TYPE_UINT32);

    // Draw all axes
    vkCmdDrawIndexed(cmdBuffer, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
}

GizmoAxis TransformGizmo::testRayHit(const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec3& gizmoPos) const {
    float closestT = std::numeric_limits<float>::max();
    GizmoAxis closestAxis = GizmoAxis::None;

    // Test X axis
    float t;
    if (rayIntersectsCylinder(rayOrigin, rayDir, gizmoPos, gizmoPos + glm::vec3(arrowLength, 0, 0), hitRadius, t)) {
        if (t < closestT) {
            closestT = t;
            closestAxis = GizmoAxis::X;
        }
    }

    // Test Y axis
    if (rayIntersectsCylinder(rayOrigin, rayDir, gizmoPos, gizmoPos + glm::vec3(0, arrowLength, 0), hitRadius, t)) {
        if (t < closestT) {
            closestT = t;
            closestAxis = GizmoAxis::Y;
        }
    }

    // Test Z axis
    if (rayIntersectsCylinder(rayOrigin, rayDir, gizmoPos, gizmoPos + glm::vec3(0, 0, arrowLength), hitRadius, t)) {
        if (t < closestT) {
            closestT = t;
            closestAxis = GizmoAxis::Z;
        }
    }

    return closestAxis;
}

bool TransformGizmo::rayIntersectsCylinder(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                                           const glm::vec3& cylinderStart, const glm::vec3& cylinderEnd,
                                           float radius, float& t) const {
    // Simplified: Use capsule-like intersection by testing distance to line segment
    glm::vec3 d = cylinderEnd - cylinderStart;
    glm::vec3 m = rayOrigin - cylinderStart;
    glm::vec3 n = rayDir;

    float dd = glm::dot(d, d);
    float nd = glm::dot(n, d);
    float mn = glm::dot(m, n);
    float md = glm::dot(m, d);
    float mm = glm::dot(m, m);

    // Check if ray is parallel to cylinder axis
    float a = dd - nd * nd;
    if (std::abs(a) < 0.0001f) {
        // Ray parallel to cylinder - check if inside
        float distSq = mm - md * md / dd;
        if (distSq <= radius * radius) {
            t = -mn; // Approximate
            return t > 0;
        }
        return false;
    }

    float b = dd * mn - nd * md;
    float c = dd * (mm - radius * radius) - md * md;

    float discriminant = b * b - a * c;
    if (discriminant < 0) return false;

    t = (-b - std::sqrt(discriminant)) / a;
    if (t < 0) {
        t = (-b + std::sqrt(discriminant)) / a;
    }

    if (t < 0) return false;

    // Check if hit is within cylinder length
    float hitParam = (md + t * nd) / dd;
    if (hitParam < 0 || hitParam > 1) return false;

    return true;
}

void TransformGizmo::beginDrag(GizmoAxis axis, const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                               const glm::vec3& objectPos, const glm::vec3& cameraPos) {
    dragging = true;
    dragAxis = axis;
    dragStartPos = objectPos;
    currentDragPos = objectPos;

    // Get axis direction
    glm::vec3 axisDir;
    switch (axis) {
        case GizmoAxis::X: axisDir = glm::vec3(1, 0, 0); break;
        case GizmoAxis::Y: axisDir = glm::vec3(0, 1, 0); break;
        case GizmoAxis::Z: axisDir = glm::vec3(0, 0, 1); break;
        default: return;
    }

    // Create constraint plane perpendicular to view direction, containing the axis
    // This is how Blender/Unity do it for precise dragging
    glm::vec3 toObject = objectPos - cameraPos;
    glm::vec3 viewDir = glm::normalize(toObject);

    // Plane normal is perpendicular to both axis and view direction
    dragPlaneNormal = glm::cross(axisDir, viewDir);
    float normalLen = glm::length(dragPlaneNormal);

    if (normalLen < 0.001f) {
        // Axis is aligned with view - use alternative perpendicular
        // Try crossing with world up first
        dragPlaneNormal = glm::cross(axisDir, glm::vec3(0, 1, 0));
        normalLen = glm::length(dragPlaneNormal);

        if (normalLen < 0.001f) {
            // Axis is world up, use world right
            dragPlaneNormal = glm::cross(axisDir, glm::vec3(1, 0, 0));
        }
    }
    dragPlaneNormal = glm::normalize(dragPlaneNormal);

    // Calculate initial click position on the constraint plane
    // Ray-plane intersection: t = dot(planePoint - rayOrigin, planeNormal) / dot(rayDir, planeNormal)
    float denom = glm::dot(rayDir, dragPlaneNormal);
    if (std::abs(denom) > 0.0001f) {
        float t = glm::dot(objectPos - rayOrigin, dragPlaneNormal) / denom;
        glm::vec3 hitPoint = rayOrigin + rayDir * t;

        // Calculate offset along axis from object center to click point
        glm::vec3 offset = hitPoint - objectPos;
        dragStartOffset = glm::dot(offset, axisDir);
    } else {
        dragStartOffset = 0.0f;
    }
}

glm::vec3 TransformGizmo::updateDrag(const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec3& cameraPos) {
    if (!dragging) return currentDragPos;

    // Get axis direction
    glm::vec3 axisDir;
    switch (dragAxis) {
        case GizmoAxis::X: axisDir = glm::vec3(1, 0, 0); break;
        case GizmoAxis::Y: axisDir = glm::vec3(0, 1, 0); break;
        case GizmoAxis::Z: axisDir = glm::vec3(0, 0, 1); break;
        default: return currentDragPos;
    }

    // Update constraint plane to track camera movement (for dynamic view changes)
    glm::vec3 toObject = currentDragPos - cameraPos;
    glm::vec3 viewDir = glm::normalize(toObject);

    glm::vec3 planeNormal = glm::cross(axisDir, viewDir);
    float normalLen = glm::length(planeNormal);

    if (normalLen < 0.001f) {
        planeNormal = glm::cross(axisDir, glm::vec3(0, 1, 0));
        normalLen = glm::length(planeNormal);
        if (normalLen < 0.001f) {
            planeNormal = glm::cross(axisDir, glm::vec3(1, 0, 0));
        }
    }
    planeNormal = glm::normalize(planeNormal);

    // Ray-plane intersection
    float denom = glm::dot(rayDir, planeNormal);
    if (std::abs(denom) < 0.0001f) {
        // Ray parallel to plane - keep current position
        return currentDragPos;
    }

    float t = glm::dot(currentDragPos - rayOrigin, planeNormal) / denom;
    glm::vec3 hitPoint = rayOrigin + rayDir * t;

    // Project hit point onto axis to get new position
    glm::vec3 offset = hitPoint - dragStartPos;
    float axisOffset = glm::dot(offset, axisDir);

    // Calculate new position, accounting for initial click offset
    glm::vec3 newPos = dragStartPos + axisDir * (axisOffset - dragStartOffset);

    // Update current position for next frame
    currentDragPos = newPos;

    return newPos;
}

void TransformGizmo::endDrag() {
    dragging = false;
    dragAxis = GizmoAxis::None;
}

VkBuffer TransformGizmo::getVertexBuffer() const {
    return vertexBuffer ? vertexBuffer->getBuffer() : VK_NULL_HANDLE;
}

VkBuffer TransformGizmo::getIndexBuffer() const {
    return indexBuffer ? indexBuffer->getBuffer() : VK_NULL_HANDLE;
}

} // namespace ohao
