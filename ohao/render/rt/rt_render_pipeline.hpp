#pragma once

#include "path_tracer.hpp"

namespace ohao {

class IRTRenderPipeline {
public:
    virtual ~IRTRenderPipeline() = default;

    virtual const char* getName() const = 0;
    virtual RTRenderProfile getProfile() const = 0;
    virtual RTRenderSettings getDefaultSettings() const = 0;
    virtual bool isInteractive() const = 0;
};

class RTRealtimePipeline final : public IRTRenderPipeline {
public:
    const char* getName() const override { return "RTRealtime"; }
    RTRenderProfile getProfile() const override { return RTRenderProfile::Realtime; }
    RTRenderSettings getDefaultSettings() const override { return kRealtimeRTSettings; }
    bool isInteractive() const override { return true; }
};

class RTOfflinePipeline final : public IRTRenderPipeline {
public:
    const char* getName() const override { return "RTOffline"; }
    RTRenderProfile getProfile() const override { return RTRenderProfile::Offline; }
    RTRenderSettings getDefaultSettings() const override { return kOfflineRTSettings; }
    bool isInteractive() const override { return false; }
};

} // namespace ohao
