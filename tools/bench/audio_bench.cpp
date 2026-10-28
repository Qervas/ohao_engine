// audio_bench: the mixer's budgets (sub-project 11, docs/specs/2026-10-28-audio-design.md,
// "Performance budgets").
//
//   audio_bench [--check budgets.json] [--out build/audio_bench.json] [--periods N]
//               [--reference build/serve_bench_reference.json] [--listen DIR]
//
// Step 1 measures mix_10ms_ms: the slice's bank (assets/audio/slice.audio.json) on a mixer without
// a device, 32 voices playing (positioned, occluded where the bank says so, topped up between
// periods as one-shots end), every bus under `world` with its low-pass engaged and a send to the
// return bus, then N periods (300) of 480 frames (10 ms at 48 kHz) rendered after 20 warm-ups;
// median and p95 of the time to render one period; since step 2 the mix includes the reverb on the
// return (the bank's defaultZone preset). reverb_10ms_ms (gated from step 2): the Freeverb alone,
// stereo, on noise, one preset (room); reverb_xfade_10ms_ms (reported): the same mid-crossfade, two
// cores running. bank_load_ms (gated from step 3): parsing nothing, decoding every variant of every
// sound into a fresh mixer, median of 5. The voice schedule (step 3) adds its own budgets.
//
// --listen DIR writes step1_buses.wav (48 kHz stereo float): one burst of the fight's sounds, first
// with every bus open, then with each leaf bus soloed in turn, and step1_buses.csv (the left
// channel's RMS per 10 ms, by segment) for a level plot.
//
// The machine is recorded as the other benches record it (bench_env.hpp). A run >25% slower than the
// reference or beside other processes using more than half a core is `noisy`: exit 2.
#include "audio/audio_bank.hpp"
#include "audio/audio_system.hpp"
#include "audio/reverb_node.hpp"
#include "common/harness.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include "bench_env.hpp"   // after the engine headers: it includes windows.h

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ohao;

namespace {

constexpr uint32_t kVoices = 32;
constexpr uint32_t kPeriod = 480;   // 10 ms at 48 kHz

struct Stats {
    double median{0.0}, p95{0.0}, min{0.0};
};

Stats stats(std::vector<double> v) {
    if (v.empty()) return {};
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q * double(v.size() - 1) + 0.5))]; };
    return {at(0.5), at(0.95), v.front()};
}

bool readBank(AudioBank& bank, std::string& error) {
    std::ifstream f(std::filesystem::path(OHAO_SOURCE_DIR) / "assets/audio/slice.audio.json", std::ios::binary);
    if (!f) {
        error = "cannot read assets/audio/slice.audio.json";
        return false;
    }
    const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto r = parseAudioBank(json);
    if (!r) {
        error = "slice.audio.json: " + (r.error().empty() ? std::string("?") : r.error()[0].path + ": " + r.error()[0].message);
        return false;
    }
    bank = std::move(r).value();
    return true;
}

// Is `bus` `ancestor` or below it?
bool under(const AudioBank& bank, int bus, int ancestor) {
    for (int b = bus; b >= 0; b = bank.parentOf(b))
        if (b == ancestor) return true;
    return false;
}

// Where the k-th voice of `sound` plays: around the listener at 2 to 11 m, with the bank's shape.
VoicePlacement placementFor(const AudioBank& bank, const SoundDesc& desc, uint32_t k) {
    VoicePlacement p;
    const ShapeDesc* shape = bank.shape(desc.shape);
    if (!shape) return p;   // "ear"
    const float r = 2.0f + float(k % 10), a = 0.7f * float(k);
    p.spatial = true;
    p.position = {r * std::cos(a), 1.0f, r * std::sin(a)};
    p.shape = SoundShape{shape->minDistance, shape->maxDistance, shape->rolloff, desc.occlude};
    return p;
}

// Fills the mixer up to kVoices playing, cycling through the bank's sounds.
void topUp(AudioSystem& a, uint32_t& next) {
    a.update();
    const auto& bank = a.bank();
    for (std::size_t n = a.activeSoundCount(); n < kVoices; ++n, ++next) {
        const auto s = static_cast<uint16_t>(next % bank.sounds.size());
        (void)a.play(s, next / static_cast<uint32_t>(bank.sounds.size()), placementFor(bank, bank.sounds[s], next));
    }
}

void writeWav(const std::filesystem::path& path, const std::vector<float>& stereo, uint32_t rate) {
    std::ofstream f(path, std::ios::binary);
    const auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<uint32_t>(stereo.size() * sizeof(float));
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(3); u16(2); u32(rate); u32(rate * 8); u16(8); u16(32);
    f.write("data", 4); u32(bytes);
    f.write(reinterpret_cast<const char*>(stereo.data()), bytes);
}

// One burst of the fight (the player's shots, an enemy's, impacts, a footstep, a reload, a hurt),
// rendered once with every bus open and then with each leaf bus soloed; returns false on failure.
bool listen(const AudioBank& bank, const std::filesystem::path& dir, std::string& error) {
    struct Hit { const char* sound; float at; };
    const Hit burst[] = {{"fire", 0.0f}, {"impact_concrete", 0.02f}, {"fire", 0.1f}, {"impact_metal", 0.12f},
                         {"enemy_fire", 0.3f}, {"footstep_concrete", 0.45f}, {"hurt", 0.55f},
                         {"footstep_concrete", 0.9f}, {"reload", 1.0f}, {"body_fall", 1.4f}};
    std::vector<std::string> segments{"all"};   // then every bus the burst plays on, in bank order
    for (const BusDesc& b : bank.buses)
        for (const Hit& h : burst)
            if (const int s = bank.sound(h.sound); s >= 0 && bank.sounds[s].bus == b.name) {
                segments.push_back(b.name);
                break;
            }
    constexpr float kSeconds = 2.4f;
    std::vector<float> wav;
    std::ofstream csv(dir / "step1_buses.csv", std::ios::binary);
    csv << "segment,t_ms,rms_db\n";
    for (const std::string& seg : segments) {
        AudioSystem a;
        if (!a.initialize(true) || !a.loadBank(bank, OHAO_SOURCE_DIR)) {
            error = "the listen mixer did not start";
            return false;
        }
        a.setListenerPosition({0, 1.7f, 0}, {0, 0, -1}, {0, 1, 0});
        if (seg != "all") a.setSolo(static_cast<uint8_t>(bank.bus(seg)), true);
        const uint64_t f0 = a.engineFrame();
        const uint32_t rate = a.sampleRate();
        uint32_t k = 0;
        for (const Hit& h : burst) {
            const int s = bank.sound(h.sound);
            if (s < 0) continue;
            (void)a.play(static_cast<uint16_t>(s), k, placementFor(bank, bank.sounds[s], k + 3),
                         f0 + static_cast<uint64_t>(h.at * float(rate)));
            ++k;
        }
        const auto frames = static_cast<uint32_t>(kSeconds * float(rate));
        std::vector<float> out(std::size_t(frames) * 2);
        a.render(out, frames);
        for (uint32_t p = 0; p + kPeriod <= frames; p += kPeriod) {
            double sum = 0.0;
            for (uint32_t i = p; i < p + kPeriod; ++i) sum += double(out[i * 2]) * out[i * 2];
            const double rms = std::sqrt(sum / kPeriod);
            csv << seg << ',' << (p * 1000 / rate) << ',' << 20.0 * std::log10(std::max(rms, 1e-6)) << '\n';
        }
        wav.insert(wav.end(), out.begin(), out.end());
        wav.insert(wav.end(), std::size_t(rate / 4) * 2, 0.0f);   // a quarter second between segments
    }
    writeWav(dir / "step1_buses.wav", wav, 48000);
    std::printf("  listen: %s (segments:", (dir / "step1_buses.wav").string().c_str());
    for (const auto& s : segments) std::printf(" %s", s.c_str());
    std::printf(")\n");
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string checkPath, outPath = "build/audio_bench.json", refPath = "build/serve_bench_reference.json", listenDir;
    int periods = 300;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--check" && i + 1 < argc) checkPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--reference" && i + 1 < argc) refPath = argv[++i];
        else if (a == "--periods" && i + 1 < argc) periods = std::max(1, std::atoi(argv[++i]));
        else if (a == "--listen" && i + 1 < argc) listenDir = argv[++i];
        else {
            std::fprintf(stderr, "usage: audio_bench [--check budgets.json] [--out file] [--periods N] "
                                 "[--reference file] [--listen DIR]\n");
            return 2;
        }
    }

    nlohmann::json budgets;
    int currentStep = 0;
    double maxSlowdown = 1.25, maxExternal = 50.0;
    if (!checkPath.empty()) {
        std::ifstream f(checkPath);
        const nlohmann::json doc = f ? nlohmann::json::parse(f, nullptr, false) : nlohmann::json();
        if (!f || doc.is_discarded()) {
            std::fprintf(stderr, "audio_bench: cannot read %s as JSON\n", checkPath.c_str());
            return 2;
        }
        budgets = doc.value("budgets", nlohmann::json::object());
        currentStep = doc.value("currentStep", 0);
        if (doc.contains("noise")) {
            maxSlowdown = doc["noise"].value("maxCalibrationSlowdown", maxSlowdown);
            maxExternal = doc["noise"].value("maxExternalCorePct", maxExternal);
        }
    }

    AudioBank bank;
    std::string error;
    if (!readBank(bank, error)) {
        std::fprintf(stderr, "audio_bench: %s\n", error.c_str());
        return 1;
    }
    std::vector<std::string> failures;

    if (!listenDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(listenDir, ec);
        if (!listen(bank, listenDir, error)) failures.push_back(error);
    }

    const double cal0 = bench::calibrationMs();
    bench::LoadMonitor monitor;
    monitor.start();
    log::setLevel(log::Level::Warn);

    // bank_load_ms: decode every variant into a fresh mixer.
    std::vector<double> loads;
    for (int i = 0; i < 5; ++i) {
        AudioSystem a;
        if (!a.initialize(true)) break;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = a.loadBank(bank, OHAO_SOURCE_DIR);
        loads.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        if (!ok) failures.push_back("loadBank failed on the slice bank");
    }

    // mix_10ms_ms: 32 voices, every world bus filtered and sending.
    AudioSystem a;
    const bool started = a.initialize(true) && a.loadBank(bank, OHAO_SOURCE_DIR);
    if (!started) failures.push_back("the deviceless mixer did not start");
    std::vector<double> samples;
    uint32_t fewest = kVoices, worldBuses = 0;
    double sumSquares = 0.0;
    bool finite = true;
    if (started) {
        a.setListenerPosition({0, 1.7f, 0}, {0, 0, -1}, {0, 1, 0});
        const int world = bank.bus("world");
        for (std::size_t b = 0; b < bank.buses.size(); ++b)
            if (world >= 0 && under(bank, static_cast<int>(b), world)) {
                a.setBus(static_cast<uint8_t>(b), -3.0f, 2000.0 + 500.0 * double(worldBuses), 0);
                a.setBusSend(static_cast<uint8_t>(b), 0.2f);
                ++worldBuses;
            }
        uint32_t next = 0;
        std::vector<float> out(std::size_t(kPeriod) * a.channels());
        for (int i = 0; i < 20 + periods; ++i) {
            topUp(a, next);
            fewest = std::min<uint32_t>(fewest, static_cast<uint32_t>(a.activeSoundCount()));
            const auto t0 = std::chrono::steady_clock::now();
            a.render(out, kPeriod);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (i < 20) continue;
            samples.push_back(ms);
            for (float v : out) {
                finite = finite && std::isfinite(v);
                sumSquares += double(v) * v;
            }
        }
    }
    // reverb_10ms_ms (step 2): the Freeverb alone on noise, stereo, the slice's room preset; then every
    // period mid-crossfade to outdoor (two cores running), reported beside it.
    bool reverbOk = true;
    const auto timeReverb = [&](bool crossfading) {
        std::vector<double> t;
        const ReverbDesc* room = bank.preset("room");
        const ReverbDesc* outdoor = bank.preset("outdoor");
        if (!room || !outdoor) {
            reverbOk = false;
            return t;
        }
        Reverb r(48000);
        r.setPreset(*room, 0);
        if (crossfading) r.setPreset(*outdoor, 0x7fffffffu);   // a fade that outlasts the run
        std::vector<float> in(std::size_t(kPeriod) * 2), wet(in.size());
        uint32_t s = 12345u;
        for (int i = 0; i < 20 + periods; ++i) {
            for (float& v : in) {
                s = s * 1664525u + 1013904223u;
                v = 0.3f * (float(s >> 8) / 8388608.0f - 1.0f);
            }
            const auto t0 = std::chrono::steady_clock::now();
            r.process(in.data(), wet.data(), kPeriod);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            for (float v : wet) reverbOk = reverbOk && std::isfinite(v);
            if (i >= 20) t.push_back(ms);
        }
        if (crossfading && !r.crossfading()) reverbOk = false;
        return t;
    };
    const std::vector<double> verbSamples = timeReverb(false), fadeSamples = timeReverb(true);
    if (!reverbOk) failures.push_back("the reverb was not what was measured (no room/outdoor preset, a non-finite sample, or no crossfade)");

    log::setLevel(log::Level::Info);
    double external = 0.0;
    const auto busy = monitor.stop(5.0, &external);
    const double cal1 = bench::calibrationMs();

    // A benchmark of broken code fails: 32 voices every period, audible, finite output.
    const double rms = samples.empty() ? 0.0 : std::sqrt(sumSquares / (double(samples.size()) * kPeriod * 2));
    const bool wrong = !started || fewest < kVoices || !finite || rms < 1e-3 || worldBuses == 0 || !reverbOk;
    if (started && wrong)
        failures.push_back("the mix is not what was measured: fewest voices " + std::to_string(fewest) + ", rms " +
                           std::to_string(rms) + (finite ? "" : ", non-finite samples") + ", world buses " +
                           std::to_string(worldBuses));

    const Stats mix = stats(samples), load = stats(loads);
    std::printf("audio_bench (%d periods of %u frames after 20 warm-ups; %u voices; %u world buses filtered and sending)\n",
                periods, kPeriod, kVoices, worldBuses);
    std::printf("  mix_10ms_ms        median %8.4f   p95 %8.4f   min %8.4f\n", mix.median, mix.p95, mix.min);
    std::string verdict = "reported";
    if (budgets.contains("mix_10ms_ms")) {
        const double max = budgets["mix_10ms_ms"].value("max", 0.0);
        const int from = budgets["mix_10ms_ms"].value("gatedFrom", 0);
        verdict = currentStep < from ? "pending" : mix.median <= max ? "pass" : "fail";
        if (verdict == "fail") {
            char text[160];
            std::snprintf(text, sizeof text, "mix_10ms_ms: median %.4f ms over its budget of %.4f ms", mix.median, max);
            failures.push_back(text);
        }
        std::printf("    budget %.3f ms, gated from step %d: %s\n", max, from, verdict.c_str());
    }
    std::printf("  bank_load_ms       median %8.3f   (%zu sounds)\n", load.median, bank.sounds.size());
    std::string loadVerdict = "reported";
    if (budgets.contains("bank_load_ms")) {
        const double max = budgets["bank_load_ms"].value("max", 0.0);
        const int from = budgets["bank_load_ms"].value("gatedFrom", 0);
        loadVerdict = currentStep < from ? "pending" : load.median <= max ? "pass" : "fail";
        if (loadVerdict == "fail") {
            char text[160];
            std::snprintf(text, sizeof text, "bank_load_ms: median %.3f ms over its budget of %.3f ms", load.median, max);
            failures.push_back(text);
        }
        std::printf("    budget %.0f ms, gated from step %d: %s\n", max, from, loadVerdict.c_str());
    }
    const Stats verb = stats(verbSamples), fade = stats(fadeSamples);
    std::printf("  reverb_10ms_ms     median %8.4f   p95 %8.4f   (one preset)\n", verb.median, verb.p95);
    std::string verbVerdict = "reported";
    if (budgets.contains("reverb_10ms_ms")) {
        const double max = budgets["reverb_10ms_ms"].value("max", 0.0);
        const int from = budgets["reverb_10ms_ms"].value("gatedFrom", 0);
        verbVerdict = currentStep < from ? "pending" : verb.median <= max ? "pass" : "fail";
        if (verbVerdict == "fail") {
            char text[160];
            std::snprintf(text, sizeof text, "reverb_10ms_ms: median %.4f ms over its budget of %.4f ms", verb.median, max);
            failures.push_back(text);
        }
        std::printf("    budget %.3f ms, gated from step %d: %s\n", max, from, verbVerdict.c_str());
    }
    std::printf("  reverb_xfade_10ms_ms median %6.4f   p95 %8.4f   (mid-crossfade: two cores; reported)\n", fade.median,
                fade.p95);
    for (const auto& [name, b] : budgets.items())
        if (name != "mix_10ms_ms" && name != "bank_load_ms" && name != "reverb_10ms_ms")
            std::printf("  %-18s not measured yet (gated from step %d)\n", name.c_str(), b.value("gatedFrom", 0));

    const double ref = bench::readReference(refPath);
    const double slow = ref > 0 ? std::max(cal0, cal1) / ref : 0.0;
    const bool noisyCal = ref > 0 && slow > maxSlowdown;
    const bool noisyLoad = external > maxExternal;
    const bool noisy = noisyCal || noisyLoad;
    std::printf("  machine: calibration before %.3f ms, after %.3f ms, reference %.3f ms (x%.2f); other processes %.1f%% of a core\n",
                cal0, cal1, ref, slow, external);
    for (const auto& p : busy) std::printf("        %-40s %6.1f%%\n", p.name.c_str(), p.corePct);

    nlohmann::json out;
    out["mix_10ms_ms"] = {{"median", mix.median}, {"p95", mix.p95}, {"min", mix.min}, {"samples", samples},
                          {"verdict", verdict}, {"voices", kVoices}, {"fewestVoices", fewest},
                          {"worldBuses", worldBuses}, {"rms", rms}};
    out["bank_load_ms"] = {{"median", load.median}, {"samples", loads}, {"sounds", bank.sounds.size()}, {"verdict", loadVerdict}};
    out["reverb_10ms_ms"] = {{"median", verb.median}, {"p95", verb.p95}, {"min", verb.min}, {"verdict", verbVerdict}};
    out["reverb_xfade_10ms_ms"] = {{"median", fade.median}, {"p95", fade.p95}, {"min", fade.min}};
    nlohmann::json busyJson = nlohmann::json::array();
    for (const auto& p : busy) busyJson.push_back({{"name", p.name}, {"corePct", p.corePct}});
    out["machine"] = {{"calibrationBeforeMs", cal0}, {"calibrationAfterMs", cal1}, {"referenceMs", ref},
                      {"externalCorePct", external}, {"busy", busyJson}};
    out["noisy"] = noisy;
    out["checked"] = !checkPath.empty();
    out["passed"] = failures.empty();
    out["failures"] = failures;
    if (const auto dir = std::filesystem::path(outPath).parent_path(); !dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    std::ofstream(outPath, std::ios::binary) << out.dump(2) << '\n';

    for (const auto& f : failures) std::printf("  FAIL: %s\n", f.c_str());
    if (noisy)
        std::printf("  NOISY: %s%s%s; the timings above are not a verdict on the code\n",
                    noisyCal ? "calibration over the reference by more than 25%" : "",
                    noisyCal && noisyLoad ? ", and " : "", noisyLoad ? "other processes were busy" : "");
    std::printf("%s (results in %s)\n", noisy ? "NOISY" : failures.empty() ? "PASS" : "FAIL", outPath.c_str());
    std::string line = "{\"suite\":\"audio_bench\",\"passed\":" + std::to_string(failures.empty() && !noisy ? 1 : 0) +
                       ",\"failed\":" + std::to_string(failures.empty() && !noisy ? 0 : 1) + ",\"failures\":[";
    if (noisy) failures.push_back("noisy run (see the machine line above)");
    for (std::size_t i = 0; i < failures.size(); ++i) line += (i ? "," : "") + test::jsonString(failures[i]);
    line += "]";
    if (noisy) line += ",\"noisy\":true";
    std::printf("%s}\n", line.c_str());
    if (wrong) return 1;   // broken code is no machine's noise
    if (noisy) return 2;
    return failures.empty() ? 0 : 1;
}
