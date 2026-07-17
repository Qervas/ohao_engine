#pragma once

// Path-tracer image formation — thin wrap of RenderSession (existing inverse path).

#include "inverse/backend/image_formation.hpp"
#include "inverse/render_session.hpp"

#include <string>
#include <string_view>

namespace ohao::inverse {

class PtImageFormation final : public IInverseImageFormation {
public:
    explicit PtImageFormation(RenderSession& session) noexcept : session_(session) {}

    [[nodiscard]] InverseBackend backend() const noexcept override {
        return InverseBackend::PathTrace;
    }

    [[nodiscard]] ImageRGBA8 forward(const InverseRenderRequest& req) override {
        return session_.render(req.viewIndex, req.budget, req.seed, req.denoise);
    }

    void rebindEnv(std::string_view path) override {
        session_.rebindEnv(std::string(path));
    }

    [[nodiscard]] RenderSession& session() noexcept { return session_; }
    [[nodiscard]] const RenderSession& session() const noexcept { return session_; }

private:
    RenderSession& session_;
};

} // namespace ohao::inverse
