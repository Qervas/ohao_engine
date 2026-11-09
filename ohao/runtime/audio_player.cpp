#include "runtime/audio_player.hpp"

#include "core/log.hpp"
#include "ecs/scene_view.hpp"
#include "ecs/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace ohao::runtime {

void AudioPlayer::initialize(const std::filesystem::path& root, AudioSystem& audio) {
    m_root = root;
    m_audio = &audio;
}

uint32_t AudioPlayer::stepFrames() const noexcept {
    const uint32_t rate = m_audio && m_audio->isInitialized() ? m_audio->sampleRate() : 48000;
    return rate / 60;
}

uint32_t AudioPlayer::horizonFrames() const noexcept { return stepFrames(); }   // one step (OPEN ruling 7)

std::optional<SoundHandle> AudioPlayer::handleOf(uint32_t voice) const {
    const auto it = m_voices.find(voice);
    if (it == m_voices.end()) return std::nullopt;
    return it->second;
}

void AudioPlayer::useBank(const AudioBank& bank) {
    if (!ready()) return;
    m_bank = bank;
    m_hasBank = true;
    m_audio->loadBank(bank, m_root);
    m_voices.clear();
    m_bed = INVALID_SOUND_HANDLE;
    m_bedName.clear();
    m_zoneName.clear();
    m_snapshotName.clear();
    m_snapshotDb.assign(bank.buses.size(), 0.0f);
    m_snapshotHz.assign(bank.buses.size(), 0.0);
    m_duckDb.assign(bank.buses.size(), 0.0f);
    m_voiceBus.clear();
    // A bus sends to the reverb at the largest send of its sounds (a sound's own send per voice is
    // not in the mixer yet).
    for (std::size_t b = 0; b < bank.buses.size(); ++b) {
        float send = 0.0f;
        for (const SoundDesc& s : bank.sounds)
            if (s.bus == bank.buses[b].name) send = std::max(send, s.send);
        if (send > 0.0f) m_audio->setBusSend(static_cast<uint8_t>(b), send);
    }
    applyOverrides(0);   // the session's audio.mix, kept across banks
}

void AudioPlayer::anchor(uint64_t step) {
    m_s0 = step;
    m_f0 = m_audio->engineFrame();
    m_anchored = true;
    ++m_anchors;
}

uint64_t AudioPlayer::startFrameOf(uint64_t step) {
    const uint64_t now = m_audio->engineFrame();
    if (!m_anchored) return now + horizonFrames();   // lockstep or paused: at once, plus the horizon
    const int64_t at = static_cast<int64_t>(m_f0) + (static_cast<int64_t>(step) - static_cast<int64_t>(m_s0)) * stepFrames() +
                       static_cast<int64_t>(horizonFrames());
    if (at < static_cast<int64_t>(now)) {   // its time has passed: at once, counted
        ++m_late;
        return now;
    }
    return static_cast<uint64_t>(at);
}

float AudioPlayer::levelDb(std::size_t b) const noexcept {
    if (b < m_overrideDb.size() && m_overrideDb[b]) return *m_overrideDb[b];
    const float snapshot = b < m_forcedDb.size() ? m_forcedDb[b] : (b < m_snapshotDb.size() ? m_snapshotDb[b] : 0.0f);
    return snapshot + (b < m_duckDb.size() ? m_duckDb[b] : 0.0f);
}

double AudioPlayer::levelHz(std::size_t b) const noexcept {
    if (b < m_forcedHz.size()) return m_forcedHz[b];
    return b < m_snapshotHz.size() ? m_snapshotHz[b] : 0.0;
}

void AudioPlayer::applyBuses(uint32_t rampFrames) {
    for (std::size_t b = 0; b < m_bank.buses.size(); ++b)
        m_audio->setBus(static_cast<uint8_t>(b), levelDb(b), levelHz(b), rampFrames);
}

void AudioPlayer::setOverrides(MixOverrides mix) {
    m_mix = std::move(mix);
    if (ready() && m_hasBank) applyOverrides(ecs::kStealFadeFrames);
}

void AudioPlayer::applyOverrides(uint32_t rampFrames) {
    const std::size_t n = m_bank.buses.size();
    m_overrideDb.assign(n, std::nullopt);
    m_muted.assign(n, 0);
    m_soloed.assign(n, 0);
    for (const MixOverrides::Bus& o : m_mix.buses) {
        const int b = m_bank.bus(o.name);
        if (b < 0) continue;
        m_overrideDb[static_cast<std::size_t>(b)] = o.db;
        m_muted[static_cast<std::size_t>(b)] = o.mute;
        m_soloed[static_cast<std::size_t>(b)] = o.solo;
    }
    m_forcedDb.clear();
    m_forcedHz.clear();
    for (const SnapshotDesc& s : m_bank.snapshots) {
        if (m_mix.snapshot.empty() || s.name != m_mix.snapshot) continue;
        m_forcedDb.assign(n, 0.0f);
        m_forcedHz.assign(n, 0.0);
        for (const auto& [name, level] : s.buses)
            if (const int b = m_bank.bus(name); b >= 0) {
                m_forcedDb[static_cast<std::size_t>(b)] = level.db;
                m_forcedHz[static_cast<std::size_t>(b)] = level.lowPassHz;
            }
    }
    for (std::size_t b = 0; b < n; ++b) {
        m_audio->setMute(static_cast<uint8_t>(b), m_muted[b] != 0);
        m_audio->setSolo(static_cast<uint8_t>(b), m_soloed[b] != 0);
    }
    applyBuses(rampFrames);
}

std::vector<DeviceBus> AudioPlayer::deviceBuses() const {
    std::vector<DeviceBus> out;
    if (!m_hasBank) return out;
    out.resize(m_bank.buses.size());
    for (std::size_t b = 0; b < out.size(); ++b) {
        out[b].db = levelDb(b);
        out[b].muted = b < m_muted.size() && m_muted[b];
        out[b].soloed = b < m_soloed.size() && m_soloed[b];
    }
    for (const auto& [voice, handle] : m_voices) {
        const auto bus = m_voiceBus.find(voice);
        if (bus != m_voiceBus.end() && bus->second < out.size() && m_audio->isPlaying(handle)) ++out[bus->second].voices;
    }
    return out;
}

void AudioPlayer::play(const ecs::AudioPlay& c, uint64_t step) {
    const int si = m_bank.sound(c.sound);
    if (si < 0) return;
    const auto sound = static_cast<uint16_t>(si);
    const SoundDesc& d = m_bank.sounds[sound];
    if (m_audio->variantCount(sound) == 0) {   // no file in any folder: skipped, warned once
        if (m_missing.insert(d.name).second)
            OHAO_LOG(Warn, audio, "audio player: sound '{}' has no file in the bank's folders; it plays nothing", d.name);
        return;
    }
    const uint64_t start = startFrameOf(step);
    if (c.outcome == ecs::AudioOutcome::Dropped) return;
    VoicePlacement p;
    p.spatial = c.pos.has_value();
    p.position = c.pos.value_or(glm::vec3(0.0f));
    if (const ShapeDesc* shape = m_bank.shape(d.shape)) p.shape = SoundShape{shape->minDistance, shape->maxDistance, shape->rolloff, false};
    p.volume = d.volume > 0.0f ? c.gain / d.volume : 0.0f;
    p.occlusionDb = c.occlusionDb;
    p.occlusionLowPassHz = c.lowPassHz;
    const SoundHandle h = m_audio->play(sound, c.variant, p, start);
    if (!isValidSoundHandle(h)) return;
    m_voices[c.voice] = h;
    m_voiceBus[c.voice] = static_cast<uint8_t>(std::max(0, m_bank.bus(d.bus)));
    ++m_played;
}

void AudioPlayer::zone(const ecs::AudioZone& c) {
    const uint32_t blend = c.blendSteps * stepFrames();
    if (c.zone != m_zoneName) {
        m_zoneName = c.zone;
        if (const ReverbDesc* preset = m_bank.preset(c.zone)) m_audio->setReverb(*preset, blend);
    }
    if (c.bed == m_bedName) return;
    if (isValidSoundHandle(m_bed)) m_audio->stop(m_bed, blend);
    m_bed = INVALID_SOUND_HANDLE;
    m_bedName = c.bed;
    const int bed = c.bed.empty() ? -1 : m_bank.sound(c.bed);
    if (bed < 0) return;
    if (m_audio->variantCount(static_cast<uint16_t>(bed)) == 0) {
        if (m_missing.insert(c.bed).second)
            OHAO_LOG(Warn, audio, "audio player: bed '{}' has no file in the bank's folders; it plays nothing", c.bed);
        return;
    }
    VoicePlacement p;
    p.loop = true;
    p.fadeInFrames = blend;
    m_bed = m_audio->play(static_cast<uint16_t>(bed), 0, p, 0);
}

void AudioPlayer::clock(uint64_t step, bool realtime) {
    // Anchored while the timeline runs in real time; again after a step back (a restore, a restart) or a
    // drift over a step from the device clock.
    if (!realtime) {
        m_anchored = false;
    } else if (!m_anchored || step < m_lastStep) {
        anchor(step);
    } else {
        const int64_t expected = static_cast<int64_t>(m_f0) + (static_cast<int64_t>(step) - static_cast<int64_t>(m_s0)) * stepFrames();
        if (std::llabs(expected - static_cast<int64_t>(m_audio->engineFrame())) > static_cast<int64_t>(stepFrames())) anchor(step);
    }
    m_lastStep = step;
}

void AudioPlayer::snapshot(const ecs::AudioSnapshot& c) {
    if (c.snapshot == m_snapshotName) return;   // already there: nothing (a restore's step after its resync)
    m_snapshotName = c.snapshot;
    std::fill(m_snapshotDb.begin(), m_snapshotDb.end(), 0.0f);
    std::fill(m_snapshotHz.begin(), m_snapshotHz.end(), 0.0);
    for (const SnapshotDesc& s : m_bank.snapshots) {
        if (s.name != c.snapshot) continue;
        for (const auto& [name, level] : s.buses)
            if (const int b = m_bank.bus(name); b >= 0) {
                m_snapshotDb[static_cast<std::size_t>(b)] = level.db;
                m_snapshotHz[static_cast<std::size_t>(b)] = level.lowPassHz;
            }
    }
    applyBuses(c.blendSteps * stepFrames());
}

void AudioPlayer::stopWorld(uint32_t blend) {
    const int world = m_bank.bus("world");
    const auto underWorld = [&](uint8_t b) {
        if (world < 0) return !m_bank.buses[b].isReturn;
        for (int at = b; at >= 0; at = m_bank.parentOf(at))
            if (at == world) return true;
        return false;
    };
    for (auto it = m_voices.begin(); it != m_voices.end();) {
        const auto bus = m_voiceBus.find(it->first);
        if (bus != m_voiceBus.end() && bus->second < m_bank.buses.size() && underWorld(bus->second)) {
            m_audio->stop(it->second, blend);
            it = m_voices.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioPlayer::update(const ecs::World& world, bool realtime) {
    const ecs::AudioSchedule* schedule = ecs::audioSchedule(world);
    if (!schedule || !ready()) return;
    if (schedule->bankVersion() != m_bankVersion) {
        m_bankVersion = schedule->bankVersion();
        useBank(schedule->bank());
    }
    // Panning follows the frame's camera (the schedule's choices used the step's listener).
    if (const auto view = ecs::resolveSceneView(world).camera) {
        const float p = glm::radians(view->pitchDeg), y = glm::radians(view->yawDeg);
        const glm::vec3 forward(std::cos(y) * std::cos(p), std::sin(p), std::sin(y) * std::cos(p));
        m_audio->setListenerPosition(view->position, forward, glm::vec3(0.0f, 1.0f, 0.0f));
    }
    // The schedule's events since the cursor, in the order it wrote them.
    const ecs::EventLog& log = world.events();
    if (!std::exchange(m_started, true)) m_cursor = log.nextSeq() - 1;   // nothing from before
    namespace ae = ecs::audio_events;
    const auto* plays = log.find<ecs::AudioPlay>(ae::kPlay);
    const auto* stops = log.find<ecs::AudioStop>(ae::kStop);
    const auto* buses = log.find<ecs::AudioBus>(ae::kBus);
    const auto* snapshots = log.find<ecs::AudioSnapshot>(ae::kSnapshot);
    const auto* zones = log.find<ecs::AudioZone>(ae::kZone);
    const auto* occlusions = log.find<ecs::AudioOcclusion>(ae::kOcclusion);
    const auto* stopWorlds = log.find<ecs::AudioStopWorld>(ae::kStopWorld);
    if (m_hasBank) clock(world.steps(), realtime);
    m_cursor = log.visit(m_cursor, SIZE_MAX, [&](const ecs::EventChannelBase& c, std::size_t i) {
        if (!m_hasBank) return;
        if (&c == plays) {
            play(plays->at(i).value, c.meta(i).step);
        } else if (&c == stops) {
            if (const auto it = m_voices.find(stops->at(i).value.voice); it != m_voices.end()) {
                m_audio->stop(it->second, ecs::kStealFadeFrames);
                m_voices.erase(it);
            }
        } else if (&c == buses) {
            const ecs::AudioBus& b = buses->at(i).value;
            if (const int bus = m_bank.bus(b.bus); bus >= 0 && static_cast<std::size_t>(bus) < m_duckDb.size()) {
                const auto at = static_cast<uint8_t>(bus);
                m_duckDb[at] = b.db;
                m_audio->setBus(at, levelDb(at), levelHz(at), b.blendSteps * stepFrames());
            }
        } else if (&c == snapshots) {
            snapshot(snapshots->at(i).value);
        } else if (&c == zones) {
            zone(zones->at(i).value);
        } else if (&c == occlusions) {
            const ecs::AudioOcclusion& o = occlusions->at(i).value;
            if (const auto it = m_voices.find(o.voice); it != m_voices.end())
                m_audio->setVoiceOcclusion(it->second, o.occlusionDb, o.lowPassHz, o.blendSteps * stepFrames());
        } else if (&c == stopWorlds) {
            stopWorld(stopWorlds->at(i).value.blendSteps * stepFrames());
        }
    });
    m_audio->update();   // finished sounds free their voices
    std::erase_if(m_voices, [&](const auto& v) { return !m_audio->voice(v.second); });
}

} // namespace ohao::runtime
