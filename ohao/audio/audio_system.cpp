#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio_system.hpp"
#include "audio/reverb_node.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace ohao {

void MaEngineDeleter::operator()(ma_engine* engine) const {
    if (!engine) return;
    ma_engine_uninit(engine);
    delete engine;
}

void MaSoundDeleter::operator()(ma_sound* sound) const {
    if (!sound) return;
    ma_sound_uninit(sound);
    delete sound;
}

// ---- a bank voice's data source ------------------------------------------------------------
//
// `delay` frames of silence, then the decoded data, ended early by a stop's linear fade. Both are
// ours because miniaudio 0.11.21's own get them wrong: a start time inside a processed block lands
// at (block end - start) frames into it instead of (start - block begin) (ma_node_read_pcm_frames),
// and ma_sound_stop_with_fade_in_pcm_frames cut a voice at once here.

struct AudioVoiceSource {
    ma_data_source_base base;   // first: miniaudio treats the source as its base
    const float* pcm{nullptr};
    ma_uint64 frames{0};
    ma_uint32 channels{0};
    ma_uint32 sampleRate{0};
    ma_uint64 cursor{0};   // data frames read
    ma_uint64 delay{0};    // silent frames left before the data
    std::atomic<uint32_t> fadeRequest{0};   // main thread -> audio thread: fade over this many frames
    uint32_t fadeLen{0}, fadeLeft{0};
    bool fading{false};

    ma_result read(float* out, ma_uint64 count, ma_uint64* read) {
        if (const uint32_t f = fadeRequest.exchange(0, std::memory_order_acquire); f != 0 && !fading) {
            fading = true;
            fadeLen = fadeLeft = f;
        }
        ma_uint64 done = 0;
        while (done < count) {
            if (fading && fadeLeft == 0) break;
            float* o = out + done * channels;
            if (delay > 0) {
                if (fading) {   // stopped before it began
                    fadeLeft = 0;
                    break;
                }
                const ma_uint64 n = std::min(delay, count - done);
                std::fill(o, o + n * channels, 0.0f);
                delay -= n;
                done += n;
                continue;
            }
            if (cursor >= frames) break;
            ma_uint64 n = std::min(frames - cursor, count - done);
            if (fading) n = std::min<ma_uint64>(n, fadeLeft);
            const float* in = pcm + cursor * channels;
            if (!fading) {
                std::copy(in, in + n * channels, o);
            } else {
                for (ma_uint64 i = 0; i < n; ++i) {
                    const float g = static_cast<float>(fadeLeft - i - 1) / static_cast<float>(fadeLen);
                    for (ma_uint32 c = 0; c < channels; ++c) o[i * channels + c] = in[i * channels + c] * g;
                }
                fadeLeft -= static_cast<uint32_t>(n);
            }
            cursor += n;
            done += n;
        }
        *read = done;
        return done == 0 ? MA_AT_END : MA_SUCCESS;
    }
};

namespace {

ma_result voiceRead(ma_data_source* ds, void* out, ma_uint64 count, ma_uint64* read) {
    return reinterpret_cast<AudioVoiceSource*>(ds)->read(static_cast<float*>(out), count, read);
}

ma_result voiceSeek(ma_data_source* ds, ma_uint64 frame) {
    auto* v = reinterpret_cast<AudioVoiceSource*>(ds);
    v->delay = 0;
    v->cursor = std::min(frame, v->frames);
    return MA_SUCCESS;
}

ma_result voiceFormat(ma_data_source* ds, ma_format* format, ma_uint32* channels, ma_uint32* rate,
                      ma_channel* map, size_t mapCap) {
    auto* v = reinterpret_cast<AudioVoiceSource*>(ds);
    *format = ma_format_f32;
    *channels = v->channels;
    *rate = v->sampleRate;
    ma_channel_map_init_standard(ma_standard_channel_map_default, map, mapCap, v->channels);
    return MA_SUCCESS;
}

ma_result voiceCursor(ma_data_source* ds, ma_uint64* cursor) {
    *cursor = reinterpret_cast<AudioVoiceSource*>(ds)->cursor;
    return MA_SUCCESS;
}

ma_result voiceLength(ma_data_source* ds, ma_uint64* length) {
    *length = reinterpret_cast<AudioVoiceSource*>(ds)->frames;
    return MA_SUCCESS;
}

ma_data_source_vtable g_voiceVtable = {voiceRead, voiceSeek, voiceFormat, voiceCursor, voiceLength, nullptr, 0};

} // namespace

void VoiceSourceDeleter::operator()(AudioVoiceSource* source) const {
    if (!source) return;
    ma_data_source_uninit(&source->base);
    delete source;
}

struct AudioLowPass {
    ma_lpf_node node;
};

void MaLpfNodeDeleter::operator()(AudioLowPass* lowPass) const {
    if (!lowPass) return;
    ma_lpf_node_uninit(&lowPass->node, nullptr);
    delete lowPass;
}

// ---- a bus's processing node ---------------------------------------------------------------
//
// One input (the bus's sound group), two outputs: 0 to the parent bus (or the endpoint), 1 the send
// to the bank's return bus (attached only while the send is above 0; its level is the output bus's
// volume). Per frame: a gain linear in dB (ramped), a second-order low-pass (Butterworth, ramped in
// log frequency, its coefficients refreshed every 16 frames), and a mute/solo gate (ramped over
// 2 ms). Parameters reach the audio thread through atomics, read once per processed block.

struct AudioBusNode {
    ma_node_base base;   // first: miniaudio treats the node as its base
    uint32_t channels{2};
    uint32_t sampleRate{48000};

    // Main thread -> audio thread.
    std::atomic<uint32_t> levelVersion{0};
    std::atomic<float> targetDb{0.0f};
    std::atomic<double> targetHz{0.0};
    std::atomic<uint32_t> rampFrames{0};
    std::atomic<uint32_t> gateVersion{0};
    std::atomic<float> gate{1.0f};

    // Audio thread.
    uint32_t levelSeen{0}, gateSeen{0};
    float curDb{0.0f}, endDb{0.0f}, dbStep{0.0f};
    uint32_t dbLeft{0};
    float gain{1.0f};
    float gateCur{1.0f}, gateEnd{1.0f}, gateStep{0.0f};
    uint32_t gateLeft{0};
    bool lpfOn{false}, lpfOffAtEnd{false};
    double hzLog{0.0}, hzEndLog{0.0}, hzStep{0.0};
    uint32_t hzLeft{0};
    float b0{1}, b1{0}, b2{0}, a1{0}, a2{0};
    float z1[8]{}, z2[8]{};

    static double openLog() { return std::log(20000.0); }
    static float dbToGain(float db) { return db <= -96.0f ? 0.0f : std::pow(10.0f, db / 20.0f); }

    void coefficients() {
        const double hz = std::min(std::exp(hzLog), 0.45 * sampleRate);
        const double w0 = 2.0 * 3.14159265358979323846 * hz / sampleRate;
        const double c = std::cos(w0), alpha = std::sin(w0) / (2.0 * 0.70710678118654752);
        const double a0 = 1.0 + alpha;
        b0 = static_cast<float>((1.0 - c) / 2.0 / a0);
        b1 = static_cast<float>((1.0 - c) / a0);
        b2 = b0;
        a1 = static_cast<float>(-2.0 * c / a0);
        a2 = static_cast<float>((1.0 - alpha) / a0);
    }

    void pull() {
        if (const uint32_t v = levelVersion.load(std::memory_order_acquire); v != levelSeen) {
            levelSeen = v;
            const float db = targetDb.load(std::memory_order_relaxed);
            const double hz = targetHz.load(std::memory_order_relaxed);
            const uint32_t ramp = rampFrames.load(std::memory_order_relaxed);
            endDb = db;
            if (ramp == 0) {
                curDb = db;
                dbLeft = 0;
                gain = dbToGain(curDb);
            } else {
                dbStep = (db - curDb) / static_cast<float>(ramp);
                dbLeft = ramp;
            }
            if (hz > 0.0) {
                if (!lpfOn) {
                    lpfOn = true;
                    hzLog = openLog();
                    std::fill(std::begin(z1), std::end(z1), 0.0f);
                    std::fill(std::begin(z2), std::end(z2), 0.0f);
                }
                lpfOffAtEnd = false;
                hzEndLog = std::log(hz);
            } else if (lpfOn) {
                lpfOffAtEnd = true;
                hzEndLog = openLog();
            }
            if (lpfOn) {
                if (ramp == 0) {
                    hzLog = hzEndLog;
                    hzLeft = 0;
                    if (lpfOffAtEnd) lpfOn = false;
                } else {
                    hzStep = (hzEndLog - hzLog) / ramp;
                    hzLeft = ramp;
                }
                coefficients();
            }
        }
        if (const uint32_t v = gateVersion.load(std::memory_order_acquire); v != gateSeen) {
            gateSeen = v;
            gateEnd = gate.load(std::memory_order_relaxed);
            gateLeft = std::max<uint32_t>(1, sampleRate / 500);   // 2 ms
            gateStep = (gateEnd - gateCur) / static_cast<float>(gateLeft);
        }
    }

    void process(const float* in, float* out0, float* out1, uint32_t frames) {
        pull();
        for (uint32_t i = 0; i < frames; ++i) {
            if (dbLeft) {
                curDb = --dbLeft ? curDb + dbStep : endDb;
                gain = dbToGain(curDb);
            }
            if (gateLeft) gateCur = --gateLeft ? gateCur + gateStep : gateEnd;
            if (lpfOn && hzLeft) {
                hzLog = --hzLeft ? hzLog + hzStep : hzEndLog;
                if (hzLeft == 0 || (hzLeft & 15u) == 0) coefficients();
                if (hzLeft == 0 && lpfOffAtEnd) lpfOn = false;
            }
            const float g = gain * gateCur;
            for (uint32_t c = 0; c < channels; ++c) {
                float x = in[i * channels + c];
                if (lpfOn) {   // transposed direct form II
                    const float y = b0 * x + z1[c];
                    z1[c] = b1 * x - a1 * y + z2[c];
                    z2[c] = b2 * x - a2 * y;
                    x = y;
                }
                const float o = x * g;
                out0[i * channels + c] = o;
                if (out1) out1[i * channels + c] = o;
            }
        }
    }
};

namespace {

void busProcess(ma_node* node, const float** in, ma_uint32* frameCountIn, float** out, ma_uint32* frameCountOut) {
    (void)frameCountIn;
    auto* bus = reinterpret_cast<AudioBusNode*>(node);
    bus->process(in[0], out[0], out[1], *frameCountOut);
}

ma_node_vtable g_busVtable = {busProcess, nullptr, 1, 2, 0};

} // namespace

// ---- the reverb on the return bus -----------------------------------------------------------
//
// One input (the return's sound group), one output (the return's bus node): the Freeverb of
// reverb_node.hpp, wet only. A preset change crosses threads through atomics and a version
// counter, picked up at the next processed block.

struct AudioReverbNode {
    ma_node_base base;   // first: miniaudio treats the node as its base
    std::unique_ptr<Reverb> reverb;
    std::atomic<uint32_t> version{0};
    std::atomic<float> room{0.5f}, damp{0.5f}, wet{0.0f};
    std::atomic<uint32_t> crossfade{0};
    uint32_t seen{0};   // audio thread

    void process(const float* in, float* out, uint32_t frames) {
        if (const uint32_t v = version.load(std::memory_order_acquire); v != seen) {
            seen = v;
            ReverbPreset p;
            p.room = room.load(std::memory_order_relaxed);
            p.damp = damp.load(std::memory_order_relaxed);
            p.wet = wet.load(std::memory_order_relaxed);
            reverb->setPreset(p, crossfade.load(std::memory_order_relaxed));
        }
        reverb->process(in, out, frames);
    }
};

namespace {

void reverbProcess(ma_node* node, const float** in, ma_uint32* frameCountIn, float** out, ma_uint32* frameCountOut) {
    (void)frameCountIn;
    reinterpret_cast<AudioReverbNode*>(node)->process(in[0], out[0], *frameCountOut);
}

ma_node_vtable g_reverbVtable = {reverbProcess, nullptr, 1, 1, 0};

} // namespace

struct AudioSystem::Bus {
    ma_sound_group group{};
    AudioBusNode node;
    std::unique_ptr<AudioReverbNode> reverb;   // the first return bus's, between its group and its node
    int parent{-1};
    bool isReturn{false};
    bool muted{false}, soloed{false};
    bool groupInit{false}, nodeInit{false}, sendAttached{false}, reverbInit{false};
};

void AudioSystem::BusDeleter::operator()(Bus* bus) const {
    if (!bus) return;
    if (bus->groupInit) ma_sound_group_uninit(&bus->group);
    if (bus->reverbInit) ma_node_uninit(&bus->reverb->base, nullptr);
    if (bus->nodeInit) ma_node_uninit(&bus->node.base, nullptr);
    delete bus;
}

struct AudioSystem::Decoded {
    void* pcm{nullptr};   // f32, interleaved, at the engine's rate
    ma_uint64 frames{0};
    ma_uint32 channels{0};
};

void AudioSystem::DecodedDeleter::operator()(Decoded* d) const {
    if (!d) return;
    if (d->pcm) ma_free(d->pcm, nullptr);
    delete d;
}

AudioSystem::AudioSystem() = default;

AudioSystem::~AudioSystem() {
    shutdown();
}

bool AudioSystem::initialize(bool withoutDevice) {
    if (m_initialized) return true;

    auto engine = MaEnginePtr(new ma_engine());

    ma_engine_config config = ma_engine_config_init();
    config.listenerCount = 1;
    if (withoutDevice) {
        config.noDevice = MA_TRUE;
        config.channels = 2;
        config.sampleRate = 48000;
    }

    const ma_result result = ma_engine_init(&config, engine.get());
    if (result != MA_SUCCESS) {
        OHAO_LOG(Error, audio, "ma_engine_init FAILED: {}", static_cast<int>(result));
        return false;
    }

    m_engine = std::move(engine);
    m_withoutDevice = withoutDevice;
    m_initialized = true;
    m_bank = defaultBank();
    buildBuses();
    return true;
}

void AudioSystem::shutdown() {
    if (!m_initialized) return;

    m_sounds.clear(); // MaSoundDeleter uninits each sound
    destroyBuses();
    m_decoded.clear();
    m_engine.reset(); // MaEngineDeleter uninits the engine
    m_initialized = false;
}

void AudioSystem::buildBuses() {
    ma_engine* engine = m_engine.get();
    ma_node_graph* graph = ma_engine_get_node_graph(engine);
    const ma_uint32 ch = ma_engine_get_channels(engine);
    const ma_uint32 rate = ma_engine_get_sample_rate(engine);
    for (const BusDesc& d : m_bank.buses) {
        std::unique_ptr<Bus, BusDeleter> bus(new Bus());
        bus->parent = m_bank.bus(d.parent);
        bus->isReturn = d.isReturn;
        bus->node.channels = ch;
        bus->node.sampleRate = rate;
        ma_node_config nc = ma_node_config_init();
        nc.vtable = &g_busVtable;
        const ma_uint32 inChannels[1] = {ch};
        const ma_uint32 outChannels[2] = {ch, ch};
        nc.pInputChannels = inChannels;
        nc.pOutputChannels = outChannels;
        if (ma_node_init(graph, &nc, nullptr, &bus->node.base) != MA_SUCCESS) {
            OHAO_LOG(Error, audio, "bus '{}': its node could not start", d.name);
            m_buses.push_back(nullptr);
            continue;
        }
        bus->nodeInit = true;
        // No pitch: a group's resampler delays what passes through it by a frame.
        if (ma_sound_group_init(engine, MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH, nullptr, &bus->group) != MA_SUCCESS) {
            OHAO_LOG(Error, audio, "bus '{}': its group could not start", d.name);
            m_buses.push_back(nullptr);
            continue;
        }
        bus->groupInit = true;
        // The first return bus carries the reverb: group -> reverb -> the bus's node (stereo only).
        const bool firstReturn = d.isReturn && ch == 2 &&
                                 std::none_of(m_buses.begin(), m_buses.end(), [](const auto& b) { return b && b->reverbInit; });
        if (firstReturn) {
            bus->reverb = std::make_unique<AudioReverbNode>();
            bus->reverb->reverb = std::make_unique<Reverb>(rate);
            ma_node_config rc = ma_node_config_init();
            rc.vtable = &g_reverbVtable;
            const ma_uint32 stereo[1] = {2};
            rc.pInputChannels = stereo;
            rc.pOutputChannels = stereo;
            if (ma_node_init(graph, &rc, nullptr, &bus->reverb->base) == MA_SUCCESS) {
                bus->reverbInit = true;
            } else {
                bus->reverb.reset();
                OHAO_LOG(Error, audio, "bus '{}': its reverb could not start; the return passes through", d.name);
            }
        }
        if (bus->reverbInit) {
            ma_node_attach_output_bus(&bus->group, 0, &bus->reverb->base, 0);
            ma_node_attach_output_bus(&bus->reverb->base, 0, &bus->node.base, 0);
        } else {
            ma_node_attach_output_bus(&bus->group, 0, &bus->node.base, 0);
        }
        Bus* parent = bus->parent >= 0 && static_cast<std::size_t>(bus->parent) < m_buses.size()
                          ? m_buses[static_cast<std::size_t>(bus->parent)].get() : nullptr;
        if (parent) ma_node_attach_output_bus(&bus->node.base, 0, &parent->group, 0);
        else ma_node_attach_output_bus(&bus->node.base, 0, ma_engine_get_endpoint(engine), 0);
        ma_node_set_output_bus_volume(&bus->node.base, 1, 0.0f);
        m_buses.push_back(std::move(bus));
    }
    // The category API's buses: by name, else the nearest stand-in (the root).
    const auto pick = [&](std::initializer_list<std::string_view> names) {
        for (std::string_view n : names)
            if (const int b = m_bank.bus(n); b >= 0) return b;
        return 0;
    };
    m_categoryBus = {pick({"sfx", "world"}), pick({"music"}), pick({"ambient", "ambience"})};
    for (int c = 0; c < 3; ++c)
        if (Bus* b = busAt(static_cast<uint8_t>(m_categoryBus[static_cast<std::size_t>(c)])); b && b->groupInit)
            ma_sound_group_set_volume(&b->group, m_categoryVolumes[static_cast<std::size_t>(c)]);
    if (Bus* root = busAt(0); root && root->groupInit) ma_sound_group_set_volume(&root->group, m_masterVolume);
}

void AudioSystem::destroyBuses() {
    while (!m_buses.empty()) m_buses.pop_back();   // children first
}

AudioSystem::Bus* AudioSystem::busAt(uint8_t bus) const {
    return bus < m_buses.size() ? m_buses[bus].get() : nullptr;
}

AudioSystem::Bus* AudioSystem::busFor(SoundCategory category) const {
    if (!isValidSoundCategory(category)) category = SoundCategory::SFX;
    return busAt(static_cast<uint8_t>(m_categoryBus[static_cast<std::size_t>(soundCategoryIndex(category))]));
}

bool AudioSystem::loadBank(const AudioBank& bank, const std::filesystem::path& root) {
    if (!m_initialized || !m_engine) return false;
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(m_mutex);
        m_sounds.clear();
    }
    destroyBuses();
    m_decoded.clear();
    m_bank = bank;
    buildBuses();

    const ma_uint32 rate = ma_engine_get_sample_rate(m_engine.get());
    m_decoded.resize(m_bank.sounds.size());
    for (std::size_t s = 0; s < m_bank.sounds.size(); ++s) {
        const SoundDesc& desc = m_bank.sounds[s];
        const auto files = resolveSoundFiles(m_bank, desc, root);
        if (files.empty()) {
            OHAO_LOG(Warn, audio, "sound '{}': no file '{}' in any folder of the bank", desc.name, desc.files);
            continue;
        }
        for (const auto& file : files) {
            ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 0, rate);
            std::unique_ptr<Decoded, DecodedDeleter> d(new Decoded());
            const std::string path = file.string();
            if (ma_decode_file(path.c_str(), &dc, &d->frames, &d->pcm) != MA_SUCCESS || !d->pcm) {
                OHAO_LOG(Warn, audio, "sound '{}': cannot decode {}", desc.name, path);
                continue;
            }
            d->channels = dc.channels;
            if (d->channels == 0) {   // the decoder kept the file's: ask it
                ma_decoder probe;
                ma_decoder_config pc = ma_decoder_config_init(ma_format_f32, 0, rate);
                if (ma_decoder_init_file(path.c_str(), &pc, &probe) == MA_SUCCESS) {
                    d->channels = probe.outputChannels;
                    ma_decoder_uninit(&probe);
                }
            }
            if (d->channels == 0) continue;
            m_decoded[s].push_back(std::move(d));
        }
    }
    // The reverb starts at the zone outside every zone.
    if (const ReverbDesc* p = m_bank.preset(m_bank.defaultZone)) setReverb(*p, 0);
    m_bankLoadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

void AudioSystem::setReverb(const ReverbPreset& preset, uint32_t crossfadeFrames) {
    for (const auto& b : m_buses) {
        if (!b || !b->reverbInit) continue;
        AudioReverbNode& r = *b->reverb;
        r.room.store(preset.room, std::memory_order_relaxed);
        r.damp.store(preset.damp, std::memory_order_relaxed);
        r.wet.store(preset.wet, std::memory_order_relaxed);
        r.crossfade.store(crossfadeFrames, std::memory_order_relaxed);
        r.version.fetch_add(1, std::memory_order_release);
        return;
    }
}

bool AudioSystem::hasReverb() const noexcept {
    return std::any_of(m_buses.begin(), m_buses.end(), [](const auto& b) { return b && b->reverbInit; });
}

uint32_t AudioSystem::variantCount(uint16_t sound) const {
    return sound < m_decoded.size() ? static_cast<uint32_t>(m_decoded[sound].size()) : 0;
}

void AudioSystem::routeOccluded(SoundInstance& instance, Bus* bus, double hz) {
    ma_node* into = bus && bus->groupInit ? static_cast<ma_node*>(&bus->group) : ma_engine_get_endpoint(m_engine.get());
    auto lowPass = MaLpfNodePtr(new AudioLowPass());
    const ma_lpf_node_config config = ma_lpf_node_config_init(
        ma_engine_get_channels(m_engine.get()), ma_engine_get_sample_rate(m_engine.get()), hz, 2);
    if (ma_lpf_node_init(ma_engine_get_node_graph(m_engine.get()), &config, nullptr, &lowPass->node) == MA_SUCCESS) {
        ma_node_attach_output_bus(&lowPass->node, 0, into, 0);
        ma_node_attach_output_bus(instance.sound.get(), 0, &lowPass->node, 0);
        instance.lowPass = std::move(lowPass);
    } else {
        delete lowPass.release();   // never initialized: freed without an uninit
        ma_node_attach_output_bus(instance.sound.get(), 0, into, 0);
        OHAO_LOG(Warn, audio, "an occluded sound plays without its low-pass (ma_lpf_node_init failed)");
    }
}

SoundHandle AudioSystem::play(uint16_t sound, uint32_t variant, const VoicePlacement& placement, uint64_t startFrame) {
    if (!m_initialized || !m_engine || sound >= m_decoded.size() || m_decoded[sound].empty()) return INVALID_SOUND_HANDLE;
    const SoundDesc& desc = m_bank.sounds[sound];
    const int busIndex = m_bank.bus(desc.bus);
    Bus* bus = busIndex >= 0 ? busAt(static_cast<uint8_t>(busIndex)) : nullptr;
    const Decoded& d = *m_decoded[sound][variant % m_decoded[sound].size()];

    SoundInstance instance;
    auto source = VoiceSourcePtr(new AudioVoiceSource());
    ma_data_source_config sc = ma_data_source_config_init();
    sc.vtable = &g_voiceVtable;
    if (ma_data_source_init(&sc, &source->base) != MA_SUCCESS) {
        delete source.release();   // never initialized: freed without an uninit
        return INVALID_SOUND_HANDLE;
    }
    source->pcm = static_cast<const float*>(d.pcm);
    source->frames = d.frames;
    source->channels = d.channels;
    source->sampleRate = ma_engine_get_sample_rate(m_engine.get());
    const uint64_t now = engineFrame();
    source->delay = startFrame > now ? startFrame - now : 0;
    instance.source = std::move(source);
    // Graded occlusion (the schedule's) when it names a low-pass, else Feel's single grade.
    const bool graded = placement.occlusionLowPassHz > 0.0;
    const bool occluded = placement.spatial && (graded || placement.shape.occluded);
    auto snd = MaSoundPtr(new ma_sound());
    // No pitch: the data is at the engine's rate, and the pitch resampler (always on otherwise)
    // delays a voice by 4 frames.
    const ma_uint32 flags = MA_SOUND_FLAG_NO_PITCH | (placement.spatial ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION);
    ma_sound_group* group = !occluded && bus && bus->groupInit ? &bus->group : nullptr;
    if (ma_sound_init_from_data_source(m_engine.get(), &instance.source->base, flags, group, snd.get()) != MA_SUCCESS) {
        OHAO_LOG(Warn, audio, "sound '{}': its voice could not start", desc.name);
        return INVALID_SOUND_HANDLE;
    }
    instance.sound = std::move(snd);
    if (placement.spatial) {
        ma_sound* s = instance.sound.get();
        ma_sound_set_position(s, placement.position.x, placement.position.y, placement.position.z);
        ma_sound_set_min_distance(s, placement.shape.minDistance);
        ma_sound_set_max_distance(s, placement.shape.maxDistance);
        ma_sound_set_rolloff(s, placement.shape.rolloff);
        ma_sound_set_attenuation_model(s, ma_attenuation_model_inverse);
    }
    if (occluded) {
        instance.occlusionHz = graded ? placement.occlusionLowPassHz : kOccludedLowPassHz;
        instance.occlusionDb = graded ? placement.occlusionDb : kOccludedGainDb;
        routeOccluded(instance, bus, instance.occlusionHz);
    }
    instance.bank = true;
    instance.baseVolume = std::max(0.0f, placement.volume * desc.volume);
    instance.spatial = placement.spatial;
    instance.position = placement.position;
    instance.shape = placement.shape;
    instance.shape.occluded = occluded;
    return start(std::move(instance));
}

void AudioSystem::stop(SoundHandle handle, uint32_t fadeFrames) {
    if (fadeFrames == 0) {
        stopSound(handle);
        return;
    }
    std::lock_guard lock(m_mutex);
    const auto it = m_sounds.find(handle);
    if (it == m_sounds.end() || !it->second.sound) return;
    if (it->second.source)
        it->second.source->fadeRequest.store(fadeFrames, std::memory_order_release);
    else
        ma_sound_stop_with_fade_in_pcm_frames(it->second.sound.get(), fadeFrames);
    it->second.stopping = true;
}

void AudioSystem::setBus(uint8_t bus, float db, double lowPassHz, uint32_t rampFrames) {
    Bus* b = busAt(bus);
    if (!b || !b->nodeInit) return;
    AudioBusNode& n = b->node;
    n.targetDb.store(db, std::memory_order_relaxed);
    n.targetHz.store(lowPassHz, std::memory_order_relaxed);
    n.rampFrames.store(rampFrames, std::memory_order_relaxed);
    n.levelVersion.fetch_add(1, std::memory_order_release);
}

void AudioSystem::setMute(uint8_t bus, bool muted) {
    if (Bus* b = busAt(bus)) {
        b->muted = muted;
        pushGates();
    }
}

void AudioSystem::setSolo(uint8_t bus, bool soloed) {
    if (Bus* b = busAt(bus)) {
        b->soloed = soloed;
        pushGates();
    }
}

void AudioSystem::pushGates() {
    const std::size_t n = m_buses.size();
    const auto ancestorOf = [&](std::size_t a, std::size_t b) {   // a is b or above it
        for (int at = static_cast<int>(b); at >= 0; at = m_buses[static_cast<std::size_t>(at)] ? m_buses[static_cast<std::size_t>(at)]->parent : -1)
            if (static_cast<std::size_t>(at) == a) return true;
        return false;
    };
    bool anySolo = false;
    for (const auto& b : m_buses) anySolo = anySolo || (b && b->soloed);
    for (std::size_t i = 0; i < n; ++i) {
        Bus* b = m_buses[i].get();
        if (!b || !b->nodeInit) continue;
        bool audible = !b->muted;
        if (audible && anySolo) {
            bool related = false;
            for (std::size_t s = 0; s < n && !related; ++s)
                if (m_buses[s] && m_buses[s]->soloed) related = ancestorOf(i, s) || ancestorOf(s, i);
            audible = related;
        }
        const float g = audible ? 1.0f : 0.0f;
        if (b->node.gate.load(std::memory_order_relaxed) != g) {
            b->node.gate.store(g, std::memory_order_relaxed);
            b->node.gateVersion.fetch_add(1, std::memory_order_release);
        }
    }
}

void AudioSystem::setBusSend(uint8_t bus, float level) {
    Bus* b = busAt(bus);
    const int ret = [&] {
        for (std::size_t i = 0; i < m_bank.buses.size(); ++i)
            if (m_bank.buses[i].isReturn) return static_cast<int>(i);
        return -1;
    }();
    Bus* r = ret >= 0 ? busAt(static_cast<uint8_t>(ret)) : nullptr;
    if (!b || !r || b == r || !b->nodeInit || !r->groupInit || bus == 0) return;
    level = std::clamp(level, 0.0f, 1.0f);
    if (level > 0.0f && !b->sendAttached) {
        ma_node_attach_output_bus(&b->node.base, 1, &r->group, 0);
        b->sendAttached = true;
    } else if (level == 0.0f && b->sendAttached) {
        ma_node_detach_output_bus(&b->node.base, 1);
        b->sendAttached = false;
    }
    ma_node_set_output_bus_volume(&b->node.base, 1, level);
}

uint64_t AudioSystem::engineFrame() const {
    return m_engine ? ma_engine_get_time_in_pcm_frames(m_engine.get()) : 0;
}

uint32_t AudioSystem::sampleRate() const {
    return m_engine ? ma_engine_get_sample_rate(m_engine.get()) : 0;
}

uint32_t AudioSystem::channels() const {
    return m_engine ? ma_engine_get_channels(m_engine.get()) : 0;
}

uint32_t AudioSystem::render(std::span<float> out, uint32_t frames) {
    if (!m_initialized || !m_withoutDevice) return 0;
    frames = std::min<uint32_t>(frames, static_cast<uint32_t>(out.size() / std::max<uint32_t>(1, channels())));
    ma_uint64 read = 0;
    ma_engine_read_pcm_frames(m_engine.get(), out.data(), frames, &read);
    return static_cast<uint32_t>(read);
}

void AudioSystem::setListenerPosition(const glm::vec3& pos, const glm::vec3& forward, const glm::vec3& up) {
    if (!m_initialized || !m_engine) return;

    m_listener = pos;
    ma_engine_listener_set_position(m_engine.get(), 0, pos.x, pos.y, pos.z);
    ma_engine_listener_set_direction(m_engine.get(), 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(m_engine.get(), 0, up.x, up.y, up.z);
}

SoundHandle AudioSystem::playSound(std::string_view path, SoundCategory category,
                                   bool loop, float volume) {
    if (!m_initialized || !m_engine || path.empty()) return INVALID_SOUND_HANDLE;
    if (!isValidSoundCategory(category)) category = SoundCategory::SFX;
    volume = clampVolume(volume);

    if (category == SoundCategory::SFX) {
        std::lock_guard lock(m_mutex);
        if (!admitVoice(audibleVolume(volume, nullptr, SoundShape{}))) return INVALID_SOUND_HANDLE;
    }

    const std::string pathStr(path);
    auto sound = MaSoundPtr(new ma_sound());
    Bus* bus = busFor(category);
    const ma_result result = ma_sound_init_from_file(
        m_engine.get(), pathStr.c_str(), 0, bus && bus->groupInit ? &bus->group : nullptr, nullptr, sound.get());
    if (result != MA_SUCCESS) {
        OHAO_LOG(Error, audio, "Failed to load '{}' (error {})", pathStr, static_cast<int>(result));
        return INVALID_SOUND_HANDLE;
    }

    ma_sound_set_spatialization_enabled(sound.get(), MA_FALSE);

    if (loop) {
        ma_sound_set_looping(sound.get(), MA_TRUE);
    }

    SoundInstance instance;
    instance.sound = std::move(sound);
    instance.category = category;
    instance.baseVolume = volume;
    instance.spatial = false;
    return start(std::move(instance));
}

SoundHandle AudioSystem::playSoundAt(std::string_view path, const glm::vec3& position,
                                     SoundCategory category, bool loop, float volume,
                                     const SoundShape& shape) {
    if (!m_initialized || !m_engine || path.empty()) return INVALID_SOUND_HANDLE;
    if (!isValidSoundCategory(category)) category = SoundCategory::SFX;
    volume = clampVolume(volume);

    if (category == SoundCategory::SFX) {
        std::lock_guard lock(m_mutex);
        if (!admitVoice(audibleVolume(volume, &position, shape))) return INVALID_SOUND_HANDLE;
    }

    const std::string pathStr(path);
    auto sound = MaSoundPtr(new ma_sound());
    Bus* bus = busFor(category);
    // Behind a wall the sound feeds a low-pass node, which feeds the bus (the cut is in the volume).
    ma_sound_group* group = !shape.occluded && bus && bus->groupInit ? &bus->group : nullptr;
    const ma_result result = ma_sound_init_from_file(
        m_engine.get(), pathStr.c_str(), 0, group, nullptr, sound.get());
    if (result != MA_SUCCESS) {
        OHAO_LOG(Error, audio, "Failed to load '{}' (error {})", pathStr, static_cast<int>(result));
        return INVALID_SOUND_HANDLE;
    }

    ma_sound_set_spatialization_enabled(sound.get(), MA_TRUE);
    ma_sound_set_position(sound.get(), position.x, position.y, position.z);
    ma_sound_set_min_distance(sound.get(), shape.minDistance);
    ma_sound_set_max_distance(sound.get(), shape.maxDistance);
    ma_sound_set_rolloff(sound.get(), shape.rolloff);
    ma_sound_set_attenuation_model(sound.get(), ma_attenuation_model_inverse);

    if (loop) {
        ma_sound_set_looping(sound.get(), MA_TRUE);
    }

    SoundInstance instance;
    instance.sound = std::move(sound);
    if (shape.occluded) routeOccluded(instance, bus);
    instance.category = category;
    instance.baseVolume = volume;
    instance.spatial = true;
    instance.position = position;
    instance.shape = shape;
    return start(std::move(instance));
}

SoundHandle AudioSystem::start(SoundInstance instance) {
    const SoundHandle handle = nextHandle();
    ma_sound* raw = instance.sound.get();
    {
        std::lock_guard lock(m_mutex);
        updateSoundVolume(instance);
        m_sounds[handle] = std::move(instance);
    }
    ma_sound_start(raw);
    return handle;
}

float AudioSystem::distanceGain(float distance, const SoundShape& shape) noexcept {
    if (shape.minDistance >= shape.maxDistance) return 1.0f;
    const float d = std::clamp(distance, shape.minDistance, shape.maxDistance);
    return shape.minDistance / (shape.minDistance + shape.rolloff * (d - shape.minDistance));
}

float AudioSystem::audibleVolume(float volume, const glm::vec3* position, const SoundShape& shape) const noexcept {
    if (!position) return volume;
    const float cut = shape.occluded ? std::pow(10.0f, kOccludedGainDb / 20.0f) : 1.0f;
    return volume * cut * distanceGain(glm::length(*position - m_listener), shape);
}

bool AudioSystem::admitVoice(float audible) {
    if (m_voiceLimit == 0) return true;
    // Finished sounds free their voices first.
    std::erase_if(m_sounds, [](auto& entry) {
        const auto& s = entry.second;
        return s.sound && !ma_sound_is_looping(s.sound.get()) && ma_sound_at_end(s.sound.get());
    });
    std::size_t voices = 0;
    auto quietest = m_sounds.end();
    float quietestAudible = 0.0f;
    for (auto it = m_sounds.begin(); it != m_sounds.end(); ++it) {
        const SoundInstance& s = it->second;
        if (s.bank || s.category != SoundCategory::SFX || !s.sound) continue;
        ++voices;
        const float a = audibleVolume(s.baseVolume, s.spatial ? &s.position : nullptr, s.shape);
        // Ties go to the older sound (the lower handle) being replaced.
        if (quietest == m_sounds.end() || a < quietestAudible || (a == quietestAudible && it->first < quietest->first)) {
            quietest = it;
            quietestAudible = a;
        }
    }
    if (voices < m_voiceLimit) return true;
    ++m_voicesDropped;
    if (quietest == m_sounds.end() || audible <= quietestAudible) return false;   // the new one is the quietest
    ma_sound_stop(quietest->second.sound.get());
    m_sounds.erase(quietest);
    return true;
}

std::size_t AudioSystem::sfxVoices() const {
    std::lock_guard lock(m_mutex);
    std::size_t n = 0;
    for (const auto& [handle, s] : m_sounds)
        n += !s.bank && s.category == SoundCategory::SFX && s.sound && ma_sound_is_playing(s.sound.get()) == MA_TRUE;
    return n;
}

std::optional<VoiceInfo> AudioSystem::voice(SoundHandle handle) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_sounds.find(handle);
    if (it == m_sounds.end() || !it->second.sound) return std::nullopt;
    const SoundInstance& s = it->second;
    VoiceInfo v;
    v.playing = ma_sound_is_playing(s.sound.get()) == MA_TRUE;
    v.spatial = s.spatial;
    v.category = s.category;
    v.position = s.position;
    v.shape = s.shape;
    v.baseVolume = s.baseVolume;
    v.volume = ma_sound_get_volume(s.sound.get());
    v.lowPassHz = s.lowPass ? (s.occlusionHz > 0.0 ? s.occlusionHz : kOccludedLowPassHz) : 0.0;
    return v;
}

void AudioSystem::stopSound(SoundHandle handle) {
    if (!isValidSoundHandle(handle)) return;
    std::lock_guard lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end() && it->second.sound) {
        ma_sound_stop(it->second.sound.get());
        m_sounds.erase(it);
    }
}

void AudioSystem::pauseSound(SoundHandle handle) {
    if (!isValidSoundHandle(handle)) return;
    std::lock_guard lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end() && it->second.sound) {
        ma_sound_stop(it->second.sound.get());
    }
}

void AudioSystem::resumeSound(SoundHandle handle) {
    if (!isValidSoundHandle(handle)) return;
    std::lock_guard lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end() && it->second.sound) {
        ma_sound_start(it->second.sound.get());
    }
}

void AudioSystem::setSoundVolume(SoundHandle handle, float volume) {
    if (!isValidSoundHandle(handle)) return;
    volume = clampVolume(volume);
    std::lock_guard lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end()) {
        it->second.baseVolume = volume;
        updateSoundVolume(it->second);
    }
}

void AudioSystem::setSoundPosition(SoundHandle handle, const glm::vec3& position) {
    if (!isValidSoundHandle(handle)) return;
    std::lock_guard lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end() && it->second.sound) {
        ma_sound_set_position(it->second.sound.get(), position.x, position.y, position.z);
    }
}

bool AudioSystem::isPlaying(SoundHandle handle) const {
    if (!isValidSoundHandle(handle)) return false;
    std::lock_guard lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it == m_sounds.end() || !it->second.sound) return false;
    return ma_sound_is_playing(it->second.sound.get()) == MA_TRUE;
}

std::size_t AudioSystem::activeSoundCount() const {
    std::lock_guard lock(m_mutex);
    return m_sounds.size();
}

// A category's volume is its bus's (the default bank: sfx, music, ambient); master is the root's.
void AudioSystem::setCategoryVolume(SoundCategory category, float volume) {
    if (!isValidSoundCategory(category)) return;
    const int idx = soundCategoryIndex(category);
    m_categoryVolumes[static_cast<std::size_t>(idx)] = clampVolume(volume);
    if (Bus* b = busFor(category); b && b->groupInit)
        ma_sound_group_set_volume(&b->group, m_categoryVolumes[static_cast<std::size_t>(idx)]);
}

float AudioSystem::getCategoryVolume(SoundCategory category) const {
    if (!isValidSoundCategory(category)) return 1.0f;
    return m_categoryVolumes[static_cast<std::size_t>(soundCategoryIndex(category))];
}

void AudioSystem::stopCategory(SoundCategory category) {
    if (!isValidSoundCategory(category)) return;
    std::lock_guard lock(m_mutex);
    std::erase_if(m_sounds, [&](auto& entry) {
        auto& instance = entry.second;
        if (!instance.bank && instance.category == category && instance.sound) {
            ma_sound_stop(instance.sound.get());
            return true;
        }
        return false;
    });
}

void AudioSystem::pauseCategory(SoundCategory category) {
    if (!isValidSoundCategory(category)) return;
    std::lock_guard lock(m_mutex);
    for (auto& [handle, instance] : m_sounds) {
        if (!instance.bank && instance.category == category && instance.sound) {
            ma_sound_stop(instance.sound.get());
        }
    }
}

void AudioSystem::resumeCategory(SoundCategory category) {
    if (!isValidSoundCategory(category)) return;
    std::lock_guard lock(m_mutex);
    for (auto& [handle, instance] : m_sounds) {
        if (!instance.bank && instance.category == category && instance.sound) {
            ma_sound_start(instance.sound.get());
        }
    }
}

void AudioSystem::setMasterVolume(float volume) {
    m_masterVolume = clampVolume(volume);
    if (Bus* root = busAt(0); root && root->groupInit) ma_sound_group_set_volume(&root->group, m_masterVolume);
}

void AudioSystem::stopAll() {
    std::lock_guard lock(m_mutex);
    for (auto& [handle, instance] : m_sounds) {
        if (instance.sound) {
            ma_sound_stop(instance.sound.get());
        }
    }
    m_sounds.clear();
}

void AudioSystem::update() {
    if (!m_initialized) return;

    std::lock_guard lock(m_mutex);
    std::erase_if(m_sounds, [](auto& entry) {
        auto& instance = entry.second;
        if (!instance.sound) return false;
        if (instance.stopping) return ma_sound_is_playing(instance.sound.get()) == MA_FALSE;
        return !ma_sound_is_looping(instance.sound.get()) && ma_sound_at_end(instance.sound.get());
    });
}

void AudioSystem::updateSoundVolume(SoundInstance& instance) {
    if (!instance.sound) return;
    const float db = instance.occlusionDb != 0.0f ? instance.occlusionDb : (instance.shape.occluded ? kOccludedGainDb : 0.0f);
    const float occlusion = db != 0.0f ? std::pow(10.0f, db / 20.0f) : 1.0f;
    ma_sound_set_volume(instance.sound.get(), instance.baseVolume * occlusion);
}

SoundHandle AudioSystem::nextHandle() {
    return m_nextHandle++;
}

} // namespace ohao
