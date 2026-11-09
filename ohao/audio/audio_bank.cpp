#include "audio/audio_bank.hpp"

#include <simdjson.h>

#include <algorithm>
#include <cmath>
#include <system_error>

namespace ohao {

namespace {

using simdjson::dom::array;
using simdjson::dom::element;
using simdjson::dom::object;

std::string esc(std::string_view key) {   // a JSON pointer token
    std::string out;
    for (char c : key) {
        if (c == '~') out += "~0";
        else if (c == '/') out += "~1";
        else out += c;
    }
    return out;
}

struct Parser {
    std::vector<BankError> errors;
    void fail(std::string path, std::string message) { errors.push_back({std::move(path), std::move(message)}); }

    void keys(object o, std::initializer_list<std::string_view> known, const std::string& path) {
        for (auto [k, v] : o) {
            (void)v;
            if (std::find(known.begin(), known.end(), k) == known.end()) fail(path + "/" + esc(k), "unknown key");
        }
    }
    bool str(element e, std::string& out, const std::string& path) {
        std::string_view s;
        if (e.get_string().get(s) != simdjson::SUCCESS) { fail(path, "not a string"); return false; }
        out = s;
        return true;
    }
    bool num(element e, float& out, const std::string& path, double lo, double hi) {
        double d;
        if (e.get_double().get(d) != simdjson::SUCCESS) { fail(path, "not a number"); return false; }
        if (!(d >= lo && d <= hi)) { fail(path, "out of range [" + fmt(lo) + ", " + fmt(hi) + "]"); return false; }
        out = static_cast<float>(d);
        return true;
    }
    bool hz(element e, double& out, const std::string& path) {
        double d;
        if (e.get_double().get(d) != simdjson::SUCCESS) { fail(path, "not a number"); return false; }
        if (!(d == 0.0 || (d >= 20.0 && d <= 24000.0))) { fail(path, "0 (none) or 20..24000 Hz"); return false; }
        out = d;
        return true;
    }
    bool uint(element e, uint32_t& out, const std::string& path, uint64_t lo, uint64_t hi) {
        int64_t v;
        if (e.get_int64().get(v) != simdjson::SUCCESS) { fail(path, "not an integer"); return false; }
        if (v < static_cast<int64_t>(lo) || v > static_cast<int64_t>(hi)) {
            fail(path, "out of range [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
            return false;
        }
        out = static_cast<uint32_t>(v);
        return true;
    }
    bool boolean(element e, bool& out, const std::string& path) {
        if (e.get_bool().get(out) != simdjson::SUCCESS) { fail(path, "not a boolean"); return false; }
        return true;
    }
    static std::string fmt(double v) {
        std::string s = std::to_string(v);
        while (s.size() > 1 && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }
};

} // namespace

int AudioBank::bus(std::string_view name) const {
    for (std::size_t i = 0; i < buses.size(); ++i)
        if (buses[i].name == name) return static_cast<int>(i);
    return -1;
}

int AudioBank::sound(std::string_view name) const {
    for (std::size_t i = 0; i < sounds.size(); ++i)
        if (sounds[i].name == name) return static_cast<int>(i);
    return -1;
}

const ShapeDesc* AudioBank::shape(std::string_view name) const {
    for (const ShapeDesc& s : shapes)
        if (s.name == name) return &s;
    return nullptr;
}

const ReverbDesc* AudioBank::preset(std::string_view name) const {
    for (const ReverbDesc& r : reverb)
        if (r.name == name) return &r;
    return nullptr;
}

int AudioBank::parentOf(int b) const {
    if (b < 0 || b >= static_cast<int>(buses.size())) return -1;
    return bus(buses[static_cast<std::size_t>(b)].parent);
}

Result<AudioBank, std::vector<BankError>> parseAudioBank(std::string_view json) {
    using R = Result<AudioBank, std::vector<BankError>>;
    simdjson::dom::parser parser;
    const simdjson::padded_string padded(json);
    element root;
    if (const auto err = parser.parse(padded).get(root); err != simdjson::SUCCESS)
        return R::err({{"", std::string("not JSON: ") + simdjson::error_message(err)}});
    object doc;
    if (root.get_object().get(doc) != simdjson::SUCCESS) return R::err({{"", "the bank is not an object"}});
    Parser p;
    AudioBank b;
    p.keys(doc, {"folders", "buses", "voices", "shapes", "sounds", "ducks", "events", "snapshots", "reverb", "defaultZone"}, "");

    if (element e; doc["folders"].get(e) == simdjson::SUCCESS) {
        array a;
        if (e.get_array().get(a) != simdjson::SUCCESS || a.size() == 0) {
            p.fail("/folders", "not a non-empty array of folders");
        } else {
            b.folders.clear();
            std::size_t i = 0;
            for (element f : a) {
                std::string s;
                if (p.str(f, s, "/folders/" + std::to_string(i))) {
                    if (s.empty()) p.fail("/folders/" + std::to_string(i), "empty");
                    else b.folders.push_back(s);
                }
                ++i;
            }
        }
    }

    // Buses: a tree under exactly one root, stored parents first.
    std::vector<BusDesc> declared;
    object buses;
    if (doc["buses"].get_object().get(buses) != simdjson::SUCCESS) {
        p.fail("/buses", "missing: an object of buses");
    } else {
        for (auto [name, value] : buses) {
            const std::string path = "/buses/" + esc(name);
            BusDesc d;
            d.name = name;
            object o;
            if (value.get_object().get(o) != simdjson::SUCCESS) { p.fail(path, "not an object"); continue; }
            p.keys(o, {"parent", "cap", "return"}, path);
            if (element e; o["parent"].get(e) == simdjson::SUCCESS) p.str(e, d.parent, path + "/parent");
            if (element e; o["cap"].get(e) == simdjson::SUCCESS) p.uint(e, d.cap, path + "/cap", 1, 256);
            if (element e; o["return"].get(e) == simdjson::SUCCESS) p.boolean(e, d.isReturn, path + "/return");
            declared.push_back(std::move(d));
        }
        const auto find = [&](std::string_view n) {
            for (std::size_t i = 0; i < declared.size(); ++i)
                if (declared[i].name == n) return static_cast<int>(i);
            return -1;
        };
        int roots = 0;
        bool treeOk = true;
        for (const BusDesc& d : declared) {
            const std::string path = "/buses/" + esc(d.name) + "/parent";
            if (d.parent.empty()) { ++roots; continue; }
            const int parent = find(d.parent);
            if (parent < 0) { p.fail(path, "no bus named '" + d.parent + "'"); treeOk = false; continue; }
            if (declared[static_cast<std::size_t>(parent)].isReturn) { p.fail(path, "a return bus has no children"); treeOk = false; }
            // A cycle: walking up from this bus comes back to it.
            int at = parent;
            for (std::size_t hops = 0; at >= 0 && hops <= declared.size(); ++hops) {
                if (declared[static_cast<std::size_t>(at)].name == d.name) {
                    p.fail(path, "the parents form a cycle");
                    treeOk = false;
                    break;
                }
                at = find(declared[static_cast<std::size_t>(at)].parent);
            }
        }
        if (roots != 1) {
            p.fail("/buses", "exactly one bus has no parent (the root); found " + std::to_string(roots));
            treeOk = false;
        }
        if (treeOk) {   // parents before children, otherwise in file order
            std::vector<bool> placed(declared.size(), false);
            while (b.buses.size() < declared.size())
                for (std::size_t i = 0; i < declared.size(); ++i)
                    if (!placed[i] && (declared[i].parent.empty() || b.bus(declared[i].parent) >= 0)) {
                        b.buses.push_back(declared[i]);
                        placed[i] = true;
                    }
        }
    }

    if (element e; doc["voices"].get(e) == simdjson::SUCCESS) p.uint(e, b.voices, "/voices", 1, 256);

    if (object shapes; doc["shapes"].get_object().get(shapes) == simdjson::SUCCESS) {
        for (auto [name, value] : shapes) {
            const std::string path = "/shapes/" + esc(name);
            ShapeDesc s;
            s.name = name;
            object o;
            if (value.get_object().get(o) != simdjson::SUCCESS) { p.fail(path, "not an object"); continue; }
            p.keys(o, {"min", "max", "rolloff"}, path);
            if (s.name == "ear") p.fail(path, "'ear' is the listener's own (not positioned): choose another name");
            if (element e; o["min"].get(e) == simdjson::SUCCESS) p.num(e, s.minDistance, path + "/min", 0.01, 10000.0);
            if (element e; o["max"].get(e) == simdjson::SUCCESS) p.num(e, s.maxDistance, path + "/max", 0.01, 100000.0);
            if (element e; o["rolloff"].get(e) == simdjson::SUCCESS) p.num(e, s.rolloff, path + "/rolloff", 0.0, 100.0);
            if (s.maxDistance <= s.minDistance) p.fail(path + "/max", "not beyond min");
            b.shapes.push_back(std::move(s));
        }
    } else if (element e; doc["shapes"].get(e) == simdjson::SUCCESS) {
        p.fail("/shapes", "not an object of shapes");
    }

    if (object sounds; doc["sounds"].get_object().get(sounds) == simdjson::SUCCESS) {
        for (auto [name, value] : sounds) {
            const std::string path = "/sounds/" + esc(name);
            SoundDesc s;
            s.name = name;
            object o;
            if (value.get_object().get(o) != simdjson::SUCCESS) { p.fail(path, "not an object"); continue; }
            p.keys(o, {"files", "bus", "priority", "shape", "holdMs", "volume", "send", "occlude"}, path);
            if (element e; o["files"].get(e) != simdjson::SUCCESS) p.fail(path + "/files", "missing: the file pattern");
            else if (p.str(e, s.files, path + "/files") && s.files.empty()) p.fail(path + "/files", "empty");
            if (element e; o["bus"].get(e) != simdjson::SUCCESS) {
                p.fail(path + "/bus", "missing");
            } else if (p.str(e, s.bus, path + "/bus")) {
                const auto it = std::find_if(declared.begin(), declared.end(), [&](const BusDesc& d) { return d.name == s.bus; });
                if (it == declared.end()) p.fail(path + "/bus", "no bus named '" + s.bus + "'");
                else if (it->isReturn) p.fail(path + "/bus", "a return bus takes sends, not sounds");
            }
            if (element e; o["priority"].get(e) == simdjson::SUCCESS) {
                uint32_t v = s.priority;
                if (p.uint(e, v, path + "/priority", 0, 255)) s.priority = static_cast<uint8_t>(v);
            }
            if (element e; o["shape"].get(e) == simdjson::SUCCESS) p.str(e, s.shape, path + "/shape");
            if (element e; o["holdMs"].get(e) == simdjson::SUCCESS) p.uint(e, s.holdMs, path + "/holdMs", 1, 600000);
            if (element e; o["volume"].get(e) == simdjson::SUCCESS) p.num(e, s.volume, path + "/volume", 0.0, 4.0);
            if (element e; o["send"].get(e) == simdjson::SUCCESS) p.num(e, s.send, path + "/send", 0.0, 1.0);
            if (element e; o["occlude"].get(e) == simdjson::SUCCESS) p.boolean(e, s.occlude, path + "/occlude");
            b.sounds.push_back(std::move(s));
        }
        for (const SoundDesc& s : b.sounds)   // shapes may be declared after the sounds
            if (s.shape != "ear" && !b.shape(s.shape))
                p.fail("/sounds/" + esc(s.name) + "/shape", "'ear' or a shape's name; no shape '" + s.shape + "'");
    } else if (element e; doc["sounds"].get(e) == simdjson::SUCCESS) {
        p.fail("/sounds", "not an object of sounds");
    }

    const auto busExists = [&](std::string_view n) {
        return std::any_of(declared.begin(), declared.end(), [&](const BusDesc& d) { return d.name == n; });
    };

    if (array ducks; doc["ducks"].get_array().get(ducks) == simdjson::SUCCESS) {
        std::size_t i = 0;
        for (element value : ducks) {
            const std::string path = "/ducks/" + std::to_string(i++);
            DuckDesc d;
            object o;
            if (value.get_object().get(o) != simdjson::SUCCESS) { p.fail(path, "not an object"); continue; }
            p.keys(o, {"on", "bus", "db", "attackSteps", "releaseSteps"}, path);
            if (element e; o["on"].get(e) != simdjson::SUCCESS) p.fail(path + "/on", "missing: the sound that ducks");
            else if (p.str(e, d.on, path + "/on") && b.sound(d.on) < 0) p.fail(path + "/on", "no sound named '" + d.on + "'");
            if (element e; o["bus"].get(e) != simdjson::SUCCESS) p.fail(path + "/bus", "missing");
            else if (p.str(e, d.bus, path + "/bus") && !busExists(d.bus)) p.fail(path + "/bus", "no bus named '" + d.bus + "'");
            if (element e; o["db"].get(e) != simdjson::SUCCESS) p.fail(path + "/db", "missing");
            else p.num(e, d.db, path + "/db", -96.0, 0.0);
            if (element e; o["attackSteps"].get(e) == simdjson::SUCCESS) p.uint(e, d.attackSteps, path + "/attackSteps", 0, 3600);
            if (element e; o["releaseSteps"].get(e) == simdjson::SUCCESS) p.uint(e, d.releaseSteps, path + "/releaseSteps", 0, 3600);
            b.ducks.push_back(std::move(d));
        }
    } else if (element e; doc["ducks"].get(e) == simdjson::SUCCESS) {
        p.fail("/ducks", "not an array of ducks");
    }

    // What a game's events play: channels by name, their cues matched in order (step 3c).
    if (array events; doc["events"].get_array().get(events) == simdjson::SUCCESS) {
        std::size_t i = 0;
        for (element value : events) {
            const std::string path = "/events/" + std::to_string(i++);
            EventSound d;
            object o;
            if (value.get_object().get(o) != simdjson::SUCCESS) { p.fail(path, "not an object"); continue; }
            p.keys(o, {"on", "by", "tag", "play", "fallback", "at", "owner"}, path);
            if (element e; o["on"].get(e) != simdjson::SUCCESS) p.fail(path + "/on", "missing: the event channel");
            else if (p.str(e, d.on, path + "/on") && d.on.empty()) p.fail(path + "/on", "empty");
            if (element e; o["by"].get(e) == simdjson::SUCCESS) {
                std::string by;
                if (p.str(e, by, path + "/by")) {
                    if (by == "player") d.by = EventSound::By::Player;
                    else if (by == "other") d.by = EventSound::By::Other;
                    else p.fail(path + "/by", "'player' or 'other'");
                }
            }
            if (element e; o["tag"].get(e) == simdjson::SUCCESS) {
                std::string tag;
                if (p.str(e, tag, path + "/tag")) d.tag = std::move(tag);
            }
            const auto known = [&](const std::string& name) {   // "{tag}" is resolved per event
                return name.find("{tag}") != std::string::npos || b.sound(name) >= 0;
            };
            if (element e; o["play"].get(e) != simdjson::SUCCESS) {
                p.fail(path + "/play", "missing: the sound, or sounds in order");
            } else if (array a; e.get_array().get(a) == simdjson::SUCCESS) {
                std::size_t k = 0;
                for (element n : a) {
                    std::string name;
                    const std::string at = path + "/play/" + std::to_string(k++);
                    if (p.str(n, name, at)) {
                        if (!known(name)) p.fail(at, "no sound named '" + name + "'");
                        d.play.push_back(std::move(name));
                    }
                }
            } else if (std::string name; p.str(e, name, path + "/play")) {
                if (!known(name)) p.fail(path + "/play", "no sound named '" + name + "'");
                d.play.push_back(std::move(name));
            }
            if (element e; o["fallback"].get(e) == simdjson::SUCCESS)
                if (p.str(e, d.fallback, path + "/fallback") && b.sound(d.fallback) < 0)
                    p.fail(path + "/fallback", "no sound named '" + d.fallback + "'");
            if (element e; o["at"].get(e) == simdjson::SUCCESS) {
                std::string at;
                if (p.str(e, at, path + "/at")) {
                    if (at == "ear") d.ear = true;
                    else if (at != "event") p.fail(path + "/at", "'ear' or 'event'");
                }
            }
            if (element e; o["owner"].get(e) == simdjson::SUCCESS) {
                std::string owner;
                if (p.str(e, owner, path + "/owner")) {
                    if (owner == "none") d.world = true;
                    else if (owner != "source") p.fail(path + "/owner", "'source' or 'none'");
                }
            }
            b.events.push_back(std::move(d));
        }
    } else if (element e; doc["events"].get(e) == simdjson::SUCCESS) {
        p.fail("/events", "not an array of event bindings");
    }

    if (object snaps; doc["snapshots"].get_object().get(snaps) == simdjson::SUCCESS) {
        for (auto [name, value] : snaps) {
            const std::string path = "/snapshots/" + esc(name);
            SnapshotDesc s;
            s.name = name;
            object o;
            if (value.get_object().get(o) != simdjson::SUCCESS) { p.fail(path, "not an object"); continue; }
            p.keys(o, {"buses", "inSteps", "outSteps"}, path);
            if (object sb; o["buses"].get_object().get(sb) == simdjson::SUCCESS) {
                for (auto [busName, bv] : sb) {
                    const std::string bp = path + "/buses/" + esc(busName);
                    if (!busExists(busName)) { p.fail(bp, "no bus named '" + std::string(busName) + "'"); continue; }
                    SnapshotBus level;
                    object bo;
                    if (bv.get_object().get(bo) != simdjson::SUCCESS) { p.fail(bp, "not an object"); continue; }
                    p.keys(bo, {"db", "lowPassHz"}, bp);
                    if (element e; bo["db"].get(e) == simdjson::SUCCESS) p.num(e, level.db, bp + "/db", -96.0, 12.0);
                    if (element e; bo["lowPassHz"].get(e) == simdjson::SUCCESS) p.hz(e, level.lowPassHz, bp + "/lowPassHz");
                    s.buses.emplace_back(std::string(busName), level);
                }
            }
            if (element e; o["inSteps"].get(e) == simdjson::SUCCESS) p.uint(e, s.inSteps, path + "/inSteps", 0, 3600);
            if (element e; o["outSteps"].get(e) == simdjson::SUCCESS) p.uint(e, s.outSteps, path + "/outSteps", 0, 3600);
            b.snapshots.push_back(std::move(s));
        }
    } else if (element e; doc["snapshots"].get(e) == simdjson::SUCCESS) {
        p.fail("/snapshots", "not an object of snapshots");
    }

    if (object verbs; doc["reverb"].get_object().get(verbs) == simdjson::SUCCESS) {
        for (auto [name, value] : verbs) {
            const std::string path = "/reverb/" + esc(name);
            ReverbDesc r;
            r.name = name;
            object o;
            if (value.get_object().get(o) != simdjson::SUCCESS) { p.fail(path, "not an object"); continue; }
            p.keys(o, {"room", "damp", "wet", "bed"}, path);
            if (element e; o["room"].get(e) == simdjson::SUCCESS) p.num(e, r.room, path + "/room", 0.0, 1.0);
            if (element e; o["damp"].get(e) == simdjson::SUCCESS) p.num(e, r.damp, path + "/damp", 0.0, 1.0);
            if (element e; o["wet"].get(e) == simdjson::SUCCESS) p.num(e, r.wet, path + "/wet", 0.0, 1.0);
            if (element e; o["bed"].get(e) == simdjson::SUCCESS && p.str(e, r.bed, path + "/bed") && b.sound(r.bed) < 0)
                p.fail(path + "/bed", "no sound named '" + r.bed + "'");
            b.reverb.push_back(std::move(r));
        }
    } else if (element e; doc["reverb"].get(e) == simdjson::SUCCESS) {
        p.fail("/reverb", "not an object of presets");
    }
    if (element e; doc["defaultZone"].get(e) == simdjson::SUCCESS && p.str(e, b.defaultZone, "/defaultZone") &&
                   std::none_of(b.reverb.begin(), b.reverb.end(), [&](const ReverbDesc& r) { return r.name == b.defaultZone; }))
        p.fail("/defaultZone", "no reverb preset named '" + b.defaultZone + "'");

    if (!p.errors.empty()) return R::err(std::move(p.errors));
    return R::ok(std::move(b));
}

AudioBank defaultBank() {
    AudioBank b;
    b.buses = {{"master", "", 0, false}, {"sfx", "master", 24, false}, {"music", "master", 0, false},
               {"ambient", "master", 0, false}};
    b.voices = 24;
    return b;
}

std::vector<std::filesystem::path> resolveSoundFiles(const AudioBank& bank, const SoundDesc& sound,
                                                     const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    const bool numbered = sound.files.find("{n}") != std::string::npos;
    const auto withExt = [](std::string s) { return fs::path(s).has_extension() ? s : s + ".wav"; };
    std::error_code ec;
    for (const std::string& folder : bank.folders) {
        std::vector<fs::path> found;
        if (numbered) {
            for (int n = 1; n < 10000; ++n) {
                std::string name = sound.files;
                name.replace(name.find("{n}"), 3, std::to_string(n));
                const fs::path file = root / folder / withExt(name);
                if (!fs::is_regular_file(file, ec)) break;
                found.push_back(file);
            }
        } else if (const fs::path file = root / folder / withExt(sound.files); fs::is_regular_file(file, ec)) {
            found.push_back(file);
        }
        if (!found.empty()) return found;
    }
    return {};
}

std::string audioBankJsonSchema() {
    return R"json({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://ohao.dev/schema/audio-bank.json",
  "title": "OHAO sound bank and mix (.audio.json)",
  "description": "docs/specs/2026-10-28-audio-design.md, 'Data: the bank and the mix'. Cross-references (a bus's parent, a sound's bus and shape, a duck's sound, a snapshot's buses, a preset's bed, defaultZone) are checked by the engine's parser.",
  "type": "object",
  "additionalProperties": false,
  "required": ["buses"],
  "properties": {
    "folders": {"type": "array", "minItems": 1, "items": {"type": "string", "minLength": 1},
                "description": "Project-relative folders searched in order; the first holding any variant of a sound supplies all of them"},
    "buses": {"type": "object", "minProperties": 1, "additionalProperties": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "parent": {"type": "string", "description": "The bus it mixes into; exactly one bus has none (the root)"},
        "cap": {"type": "integer", "minimum": 1, "maximum": 256, "description": "Voices at once on this bus"},
        "return": {"type": "boolean", "description": "Takes sends (the reverb); no sound plays on it directly"}}}},
    "voices": {"type": "integer", "minimum": 1, "maximum": 256, "description": "Voices at once, all buses together"},
    "shapes": {"type": "object", "additionalProperties": {
      "type": "object", "additionalProperties": false,
      "properties": {"min": {"type": "number"}, "max": {"type": "number"}, "rolloff": {"type": "number", "minimum": 0}}}},
    "sounds": {"type": "object", "additionalProperties": {
      "type": "object", "additionalProperties": false, "required": ["files", "bus"],
      "properties": {
        "files": {"type": "string", "minLength": 1, "description": "Pattern under each folder; {n} counts variants from 1; .wav unless named"},
        "bus": {"type": "string"},
        "priority": {"type": "integer", "minimum": 0, "maximum": 255},
        "shape": {"type": "string", "description": "'ear' (at the listener) or a shape's name"},
        "holdMs": {"type": "integer", "minimum": 1, "description": "How long it holds its voice, declared (never read from the file)"},
        "volume": {"type": "number", "minimum": 0, "maximum": 4},
        "send": {"type": "number", "minimum": 0, "maximum": 1, "description": "To the reverb"},
        "occlude": {"type": "boolean", "description": "Gets an occlusion ray"}}}},
    "ducks": {"type": "array", "items": {
      "type": "object", "additionalProperties": false, "required": ["on", "bus", "db"],
      "properties": {"on": {"type": "string"}, "bus": {"type": "string"}, "db": {"type": "number", "minimum": -96, "maximum": 0},
                     "attackSteps": {"type": "integer", "minimum": 0}, "releaseSteps": {"type": "integer", "minimum": 0}}}},
    "events": {"type": "array", "description": "What a game's events play: the first binding of a channel whose by and tag hold", "items": {
      "type": "object", "additionalProperties": false, "required": ["on", "play"],
      "properties": {
        "on": {"type": "string", "description": "An event channel (events.read's names), e.g. slice.shot"},
        "by": {"enum": ["player", "other"], "description": "The event's source is the listener's player, or not"},
        "tag": {"type": "string", "description": "The event's tag must be this (a surface, empty)"},
        "play": {"oneOf": [{"type": "string"}, {"type": "array", "items": {"type": "string"}}],
                 "description": "The sound or sounds, in order; {tag} is the event's tag (default when empty)"},
        "fallback": {"type": "string", "description": "In place of a sound the bank lacks"},
        "at": {"enum": ["ear", "event"], "description": "At the listener, or at the event's place (the default)"},
        "owner": {"enum": ["source", "none"], "description": "Whose sound: the event's source (the default), or nobody's"}}}},
    "snapshots": {"type": "object", "additionalProperties": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "buses": {"type": "object", "additionalProperties": {"type": "object", "additionalProperties": false,
                  "properties": {"db": {"type": "number", "minimum": -96, "maximum": 12},
                                 "lowPassHz": {"type": "number", "description": "0: none; else 20..24000"}}}},
        "inSteps": {"type": "integer", "minimum": 0}, "outSteps": {"type": "integer", "minimum": 0}}}},
    "reverb": {"type": "object", "additionalProperties": {
      "type": "object", "additionalProperties": false,
      "properties": {"room": {"type": "number", "minimum": 0, "maximum": 1}, "damp": {"type": "number", "minimum": 0, "maximum": 1},
                     "wet": {"type": "number", "minimum": 0, "maximum": 1}, "bed": {"type": "string"}}}},
    "defaultZone": {"type": "string", "description": "The reverb preset outside every zone"}
  }
}
)json";
}

} // namespace ohao
