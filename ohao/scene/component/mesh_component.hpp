#pragma once

#include "core/common_types.hpp"
#include "scene/component/component.hpp"
#include "scene/asset/model.hpp"

#include <cstdint>
#include <memory>

namespace ohao {

class MeshComponent : public Component {
public:
    using Ptr = std::shared_ptr<MeshComponent>;

    enum class RenderMode : std::uint8_t {
        Solid = 0,
        Wireframe = 1,
        Points = 2,
        // Legacy aliases
        SOLID = Solid,
        WIREFRAME = Wireframe,
        POINTS = Points,
    };

    MeshComponent();
    ~MeshComponent() override;

    void setModel(std::shared_ptr<Model> model);
    [[nodiscard]] std::shared_ptr<Model> getModel() const noexcept { return model; }

    void setVisible(bool visible) noexcept { this->visible = visible; }
    [[nodiscard]] bool isVisible() const noexcept { return visible; }

    void setRenderMode(RenderMode mode) noexcept { renderMode = mode; }
    [[nodiscard]] RenderMode getRenderMode() const noexcept { return renderMode; }

    [[nodiscard]] std::uint32_t getVertexCount() const;
    [[nodiscard]] std::uint32_t getIndexCount() const;
    [[nodiscard]] bool hasGeometry() const noexcept {
        return model && !model->vertices.empty() && !model->indices.empty();
    }

    [[nodiscard]] const char* getTypeName() const override;
    void initialize() override;
    void render() override;
    void destroy() override;

    // Buffer management (used by renderer) — prefer MeshBufferInfo form
    void setBufferOffsets(std::uint32_t vertexOffset, std::uint32_t indexOffset, std::uint32_t indexCount);
    void setBufferInfo(const MeshBufferInfo& info) noexcept;
    [[nodiscard]] MeshBufferInfo getBufferInfo() const noexcept;

    [[nodiscard]] std::uint32_t getVertexOffset() const noexcept { return buffer.vertexOffset; }
    [[nodiscard]] std::uint32_t getIndexOffset() const noexcept { return buffer.indexOffset; }
    [[nodiscard]] std::uint32_t getBufferIndexCount() const noexcept { return buffer.indexCount; }

private:
    std::shared_ptr<Model> model;
    bool visible{true};
    RenderMode renderMode{RenderMode::Solid};
    MeshBufferInfo buffer{};
};

} // namespace ohao
