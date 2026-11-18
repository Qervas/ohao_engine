#include "audio/module.hpp"

#include "audio/audio_schedule.hpp"

#include "modules/audio_dependencies.hpp"

namespace ohao::ecs {

// Installs the `audio_schedule` simulate system and its snapshot hook once per World (a second call returns the
// first; audio/audio_schedule.cpp).
AudioSchedule& installAudio(World& world, AssetResolver& assets);

namespace {

class AudioModule final : public Module {
public:
    [[nodiscard]] std::string_view name() const override { return "audio"; }
    [[nodiscard]] std::span<const std::string_view> dependencies() const override { return kAudioDependencies; }
    void registerTypes(reflect::TypeRegistry& types) const override { registerAudioTypes(types); }
    void install(World& world, const Services& services) const override { installAudio(world, services.assets); }
    // What others add to it: sound sources, by name.
    [[nodiscard]] std::vector<std::string> contributions(const World& world) const override {
        const AudioSchedule* s = audioSchedule(world);
        return s ? s->sourceNames() : std::vector<std::string>{};
    }
    void removeContributions(World& world, std::span<const std::string> names) const override {
        if (AudioSchedule* s = audioSchedule(world))
            for (const std::string& n : names) s->removeSource(n);
    }
};

} // namespace

const Module& audioModule() {
    static const AudioModule m;
    return m;
}

} // namespace ohao::ecs
