#pragma once

// Diff-IR image formation for inverse --backend diff.

#include "inverse/backend/image_formation.hpp"
#include "render/diff/diff_session.hpp"

namespace ohao::inverse {

class DiffImageFormation final : public IInverseImageFormation {
public:
    explicit DiffImageFormation(ohao::diff::DiffSession& session) noexcept : session_(session) {}

    [[nodiscard]] InverseBackend backend() const noexcept override {
        return InverseBackend::Diff;
    }

    [[nodiscard]] bool supportsAnalyticGrads() const noexcept override { return true; }

    [[nodiscard]] ImageRGBA8 forward(const InverseRenderRequest& req) override {
        session_.width = req.budget.width;
        session_.height = req.budget.height;
        return session_.forwardView(req.viewIndex);
    }

    [[nodiscard]] ohao::diff::DiffSession& session() noexcept { return session_; }

private:
    ohao::diff::DiffSession& session_;
};

} // namespace ohao::inverse
