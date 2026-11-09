#include "vfx/vfx_set.hpp"

#include <simdjson.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace ohao::vfx {

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

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

struct Parser {
    std::vector<VfxError> errors;
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
    // A number (min = max) or [min, max] within [lo, hi], min <= max.
    bool range(element e, Range& out, const std::string& path, double lo, double hi) {
        array a;
        if (e.get_array().get(a) == simdjson::SUCCESS) {
            if (a.size() != 2) { fail(path, "a number or [min, max]"); return false; }
            Range r;
            if (!num(a.at(0), r.min, path + "/0", lo, hi) || !num(a.at(1), r.max, path + "/1", lo, hi)) return false;
            if (r.min > r.max) { fail(path, "min > max"); return false; }
            out = r;
            return true;
        }
        float v;
        if (!num(e, v, path, lo, hi)) return false;
        out = {v, v};
        return true;
    }
    // "#rrggbb" (sRGB, converted to linear) or [r, g, b] (linear, 0..64).
    bool color(element e, glm::vec3& out, const std::string& path) {
        std::string_view s;
        if (e.get_string().get(s) == simdjson::SUCCESS) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            if (s.size() != 7 || s[0] != '#') { fail(path, "a colour: \"#rrggbb\" or [r, g, b]"); return false; }
            for (int i = 0; i < 3; ++i) {
                const int hi = hex(s[1 + 2 * i]), lo = hex(s[2 + 2 * i]);
                if (hi < 0 || lo < 0) { fail(path, "a colour: \"#rrggbb\" or [r, g, b]"); return false; }
                out[i] = srgbToLinear(static_cast<float>(hi * 16 + lo) / 255.0f);
            }
            return true;
        }
        array a;
        if (e.get_array().get(a) != simdjson::SUCCESS || a.size() != 3) { fail(path, "a colour: \"#rrggbb\" or [r, g, b]"); return false; }
        for (std::size_t i = 0; i < 3; ++i)
            if (!num(a.at(i), out[static_cast<int>(i)], path + "/" + std::to_string(i), 0.0, 64.0)) return false;
        return true;
    }
    // A colour over life: one colour; a list of 2-4 colours, evenly spaced; or 1-4 [t, colour] keys.
    bool colorCurve(element e, Curve<glm::vec3>& out, const std::string& path) {
        array a;
        bool list = false;
        if (e.get_array().get(a) == simdjson::SUCCESS && a.size() > 0) {
            element first = a.at(0);
            list = first.is_string() || first.is_array();
        }
        if (!list) {
            glm::vec3 c;
            if (!color(e, c, path)) return false;
            out.keys = {{0.0f, c}};
            return true;
        }
        if (a.size() > 4) { fail(path, "at most 4 keys"); return false; }
        out.keys.clear();
        std::size_t i = 0;
        for (element k : a) {
            const std::string kp = path + "/" + std::to_string(i);
            array pair;
            if (k.get_array().get(pair) == simdjson::SUCCESS && pair.size() == 2 && !pair.at(0).is_string()) {
                float t;
                glm::vec3 c;
                if (!num(pair.at(0), t, kp + "/0", 0.0, 1.0) || !color(pair.at(1), c, kp + "/1")) return false;
                out.keys.emplace_back(t, c);
            } else {
                glm::vec3 c;
                if (!color(k, c, kp)) return false;
                out.keys.emplace_back(a.size() == 1 ? 0.0f : static_cast<float>(i) / static_cast<float>(a.size() - 1), c);
            }
            ++i;
        }
        return ordered(out.keys, path);
    }
    // A value over life: a number; a list of 2-4 numbers, evenly spaced; or 1-4 [t, value] keys.
    bool scalarCurve(element e, Curve<float>& out, const std::string& path, double lo, double hi) {
        array a;
        if (e.get_array().get(a) != simdjson::SUCCESS) {
            float v;
            if (!num(e, v, path, lo, hi)) return false;
            out.keys = {{0.0f, v}};
            return true;
        }
        if (a.size() == 0 || a.size() > 4) { fail(path, "1 to 4 keys"); return false; }
        out.keys.clear();
        std::size_t i = 0;
        for (element k : a) {
            const std::string kp = path + "/" + std::to_string(i);
            array pair;
            if (k.get_array().get(pair) == simdjson::SUCCESS) {
                float t, v;
                if (pair.size() != 2 || !num(pair.at(0), t, kp + "/0", 0.0, 1.0) || !num(pair.at(1), v, kp + "/1", lo, hi)) {
                    if (pair.size() != 2) fail(kp, "[t, value]");
                    return false;
                }
                out.keys.emplace_back(t, v);
            } else {
                float v;
                if (!num(k, v, kp, lo, hi)) return false;
                out.keys.emplace_back(a.size() == 1 ? 0.0f : static_cast<float>(i) / static_cast<float>(a.size() - 1), v);
            }
            ++i;
        }
        return ordered(out.keys, path);
    }
    template <class T>
    bool ordered(const std::vector<std::pair<float, T>>& keys, const std::string& path) {
        for (std::size_t i = 1; i < keys.size(); ++i)
            if (keys[i].first < keys[i - 1].first) { fail(path, "keys out of order in t"); return false; }
        return true;
    }
    static std::string fmt(double v) {
        std::string s = std::to_string(v);
        while (s.size() > 1 && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }
};

template <class T>
int indexOf(const std::vector<T>& v, std::string_view name) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i].name == name) return static_cast<int>(i);
    return -1;
}

std::string substitute(std::string_view pattern, std::string_view with) {
    std::string s(pattern);
    const auto at = s.find("{tag}");
    if (at != std::string::npos) s.replace(at, 5, with);
    return s;
}

template <class T>
int resolveIn(const std::vector<T>& v, std::string_view pattern, std::string_view surface, std::string_view fallback) {
    if (pattern.find("{tag}") == std::string_view::npos) return indexOf(v, pattern);
    if (const int i = indexOf(v, substitute(pattern, surface)); i >= 0) return i;
    return fallback.empty() ? -1 : indexOf(v, substitute(pattern, fallback));
}

EmitterDesc emitterFrom(uint32_t count, float spreadRad, Range speed, Range life, float sizeStart, float sizeEnd,
                        glm::vec3 colorStart, float alphaStart, glm::vec3 colorEnd, float alphaEnd, float gravity, float drag,
                        Blend blend, bool lit) {
    EmitterDesc e;
    e.count = count;
    e.spreadDeg = spreadRad * (180.0f / 3.14159265358979f);
    e.speed = speed;
    e.life = life;
    e.sizeStart = sizeStart;
    e.sizeEnd = sizeEnd;
    e.color.keys = {{0.0f, colorStart}, {1.0f, colorEnd}};
    e.alpha.keys = {{0.0f, alphaStart}, {1.0f, alphaEnd}};
    e.gravity = gravity;
    e.drag = drag;
    e.blend = blend;
    e.lit = lit;
    return e;
}

} // namespace

float EffectDesc::maxLife() const noexcept {
    float m = 0.0f;
    for (const EmitterDesc& e : emitters) m = std::max(m, e.life.max);
    if (light) m = std::max(m, light->time);
    if (proxy) m = std::max(m, proxy->life);
    return m;
}

int VfxSet::texture(std::string_view name) const { return indexOf(textures, name); }
int VfxSet::effect(std::string_view name) const { return indexOf(effects, name); }
int VfxSet::decal(std::string_view name) const { return indexOf(decals, name); }
int VfxSet::tracer(std::string_view name) const { return indexOf(tracers, name); }

int resolveEffect(const VfxSet& set, std::string_view pattern, std::string_view surface, std::string_view fallback) {
    return resolveIn(set.effects, pattern, surface, fallback);
}

int resolveDecal(const VfxSet& set, std::string_view pattern, std::string_view surface, std::string_view fallback) {
    return resolveIn(set.decals, pattern, surface, fallback);
}

Result<VfxSet, std::vector<VfxError>> parseVfxSet(std::string_view json) {
    using R = Result<VfxSet, std::vector<VfxError>>;
    simdjson::dom::parser parser;
    const simdjson::padded_string padded(json);
    element root;
    if (const auto err = parser.parse(padded).get(root); err != simdjson::SUCCESS)
        return R::err({{"", std::string("not JSON: ") + simdjson::error_message(err)}});
    object doc;
    if (root.get_object().get(doc) != simdjson::SUCCESS) return R::err({{"", "the set is not an object"}});
    Parser p;
    VfxSet s;
    p.keys(doc, {"textures", "effects", "decals", "tracers", "events", "caps"}, "");

    if (object textures; doc["textures"].get_object().get(textures) == simdjson::SUCCESS) {
        for (auto [name, value] : textures) {
            const std::string path = "/textures/" + esc(name);
            TextureDesc t;
            t.name = name;
            object o;
            if (value.get_object().get(o) != simdjson::SUCCESS) { p.fail(path, "not an object"); continue; }
            p.keys(o, {"file", "normal", "cols", "rows"}, path);
            if (element e; o["file"].get(e) != simdjson::SUCCESS) p.fail(path + "/file", "missing");
            else if (p.str(e, t.file, path + "/file") && t.file.empty()) p.fail(path + "/file", "empty");
            if (element e; o["normal"].get(e) == simdjson::SUCCESS) p.str(e, t.normal, path + "/normal");
            if (element e; o["cols"].get(e) == simdjson::SUCCESS) p.uint(e, t.cols, path + "/cols", 1, 64);
            if (element e; o["rows"].get(e) == simdjson::SUCCESS) p.uint(e, t.rows, path + "/rows", 1, 64);
            s.textures.push_back(std::move(t));
        }
    } else if (element e; doc["textures"].get(e) == simdjson::SUCCESS) {
        p.fail("/textures", "not an object of textures");
    }

    const auto textureRef = [&](element e, int& out, const std::string& path) {
        std::string name;
        if (!p.str(e, name, path)) return;
        out = s.texture(name);
        if (out < 0) p.fail(path, "no texture named '" + name + "'");
    };

    if (object effects; doc["effects"].get_object().get(effects) == simdjson::SUCCESS) {
        for (auto [name, value] : effects) {
            const std::string path = "/effects/" + esc(name);
            EffectDesc fx;
            fx.name = name;
            object o;
            if (value.get_object().get(o) != simdjson::SUCCESS) { p.fail(path, "not an object"); continue; }
            p.keys(o, {"emitters", "light", "proxy"}, path);
            array emitters;
            if (o["emitters"].get_array().get(emitters) != simdjson::SUCCESS || emitters.size() == 0) {
                p.fail(path + "/emitters", "missing: a non-empty array of emitters");
            } else {
                std::size_t i = 0;
                for (element ev : emitters) {
                    const std::string ep = path + "/emitters/" + std::to_string(i++);
                    EmitterDesc em;
                    em.color.keys = {{0.0f, glm::vec3(1.0f)}};
                    em.alpha.keys = {{0.0f, 1.0f}};
                    object eo;
                    if (ev.get_object().get(eo) != simdjson::SUCCESS) { p.fail(ep, "not an object"); continue; }
                    p.keys(eo, {"count", "rate", "life", "speed", "size", "spread", "gravity", "drag", "offset", "color", "alpha",
                                "intensity", "blend", "lit", "soft", "texture", "frames", "sort"},
                           ep);
                    if (element e; eo["count"].get(e) == simdjson::SUCCESS) p.uint(e, em.count, ep + "/count", 0, 4096);
                    if (element e; eo["rate"].get(e) == simdjson::SUCCESS) p.num(e, em.rate, ep + "/rate", 0.0, 10000.0);
                    if (em.count == 0 && em.rate == 0.0f) p.fail(ep + "/count", "emits nothing: a count or a rate");
                    if (element e; eo["life"].get(e) == simdjson::SUCCESS) p.range(e, em.life, ep + "/life", 0.001, 60.0);
                    if (element e; eo["speed"].get(e) == simdjson::SUCCESS) p.range(e, em.speed, ep + "/speed", 0.0, 1000.0);
                    if (element e; eo["size"].get(e) == simdjson::SUCCESS) {
                        Range r;
                        array sa;
                        // [start, end] over life (either may be the larger), or one size throughout.
                        if (e.get_array().get(sa) == simdjson::SUCCESS && sa.size() == 2) {
                            if (p.num(sa.at(0), r.min, ep + "/size/0", 0.0, 100.0) && p.num(sa.at(1), r.max, ep + "/size/1", 0.0, 100.0))
                                em.sizeStart = r.min, em.sizeEnd = r.max;
                        } else if (p.range(e, r, ep + "/size", 0.0, 100.0)) {
                            em.sizeStart = r.min, em.sizeEnd = r.max;
                        }
                    }
                    if (element e; eo["spread"].get(e) == simdjson::SUCCESS) p.num(e, em.spreadDeg, ep + "/spread", 0.0, 180.0);
                    if (element e; eo["gravity"].get(e) == simdjson::SUCCESS) p.num(e, em.gravity, ep + "/gravity", -100.0, 100.0);
                    if (element e; eo["drag"].get(e) == simdjson::SUCCESS) p.num(e, em.drag, ep + "/drag", 0.0, 100.0);
                    if (element e; eo["offset"].get(e) == simdjson::SUCCESS) p.num(e, em.offset, ep + "/offset", -10.0, 10.0);
                    if (element e; eo["color"].get(e) == simdjson::SUCCESS) p.colorCurve(e, em.color, ep + "/color");
                    if (element e; eo["alpha"].get(e) == simdjson::SUCCESS) p.scalarCurve(e, em.alpha, ep + "/alpha", 0.0, 1.0);
                    if (element e; eo["intensity"].get(e) == simdjson::SUCCESS) p.num(e, em.intensity, ep + "/intensity", 0.0, 10000.0);
                    if (element e; eo["blend"].get(e) == simdjson::SUCCESS) {
                        std::string b;
                        if (p.str(e, b, ep + "/blend")) {
                            if (b == "additive") em.blend = Blend::Additive;
                            else if (b == "alpha") em.blend = Blend::Alpha;
                            else if (b == "premultiplied") em.blend = Blend::Premultiplied;
                            else p.fail(ep + "/blend", "additive, alpha or premultiplied; not '" + b + "'");
                        }
                    }
                    if (element e; eo["lit"].get(e) == simdjson::SUCCESS) p.boolean(e, em.lit, ep + "/lit");
                    if (element e; eo["soft"].get(e) == simdjson::SUCCESS) p.num(e, em.soft, ep + "/soft", 0.0, 10.0);
                    if (element e; eo["texture"].get(e) == simdjson::SUCCESS) textureRef(e, em.texture, ep + "/texture");
                    if (element e; eo["frames"].get(e) == simdjson::SUCCESS) {
                        std::string_view mode;
                        if (e.get_string().get(mode) == simdjson::SUCCESS) {
                            if (mode == "life") em.frames = FrameMode::Life;
                            else if (mode == "random") em.frames = FrameMode::Random;
                            else p.fail(ep + "/frames", "life, random or a frame's index");
                        } else if (p.uint(e, em.frame, ep + "/frames", 0, 4095)) {
                            em.frames = FrameMode::Fixed;
                        }
                    }
                    if (element e; eo["sort"].get(e) == simdjson::SUCCESS) {
                        std::string m;
                        if (p.str(e, m, ep + "/sort")) {
                            if (m == "auto") em.sort = SortMode::Auto;
                            else if (m == "none") em.sort = SortMode::None;
                            else if (m == "particles") em.sort = SortMode::Particles;
                            else p.fail(ep + "/sort", "auto, none or particles; not '" + m + "'");
                        }
                    }
                    fx.emitters.push_back(std::move(em));
                }
            }
            if (object lo; o["light"].get_object().get(lo) == simdjson::SUCCESS) {
                const std::string lp = path + "/light";
                FlashLight l;
                p.keys(lo, {"color", "intensity", "range", "time"}, lp);
                if (element e; lo["color"].get(e) == simdjson::SUCCESS) p.color(e, l.color, lp + "/color");
                if (element e; lo["intensity"].get(e) == simdjson::SUCCESS) p.num(e, l.intensity, lp + "/intensity", 0.0, 100000.0);
                if (element e; lo["range"].get(e) == simdjson::SUCCESS) p.num(e, l.range, lp + "/range", 0.01, 1000.0);
                if (element e; lo["time"].get(e) == simdjson::SUCCESS) p.num(e, l.time, lp + "/time", 0.001, 60.0);
                fx.light = l;
            } else if (element e; o["light"].get(e) == simdjson::SUCCESS) {
                p.fail(path + "/light", "not an object");
            }
            if (object po; o["proxy"].get_object().get(po) == simdjson::SUCCESS) {
                const std::string pp = path + "/proxy";
                ProxyDesc d;
                p.keys(po, {"spheres", "radius", "density", "rise", "life"}, pp);
                if (element e; po["spheres"].get(e) == simdjson::SUCCESS) p.uint(e, d.spheres, pp + "/spheres", 1, 8);
                if (element e; po["radius"].get(e) == simdjson::SUCCESS) p.range(e, d.radius, pp + "/radius", 0.01, 100.0);
                if (element e; po["density"].get(e) == simdjson::SUCCESS) p.num(e, d.density, pp + "/density", 0.0, 100.0);
                if (element e; po["rise"].get(e) == simdjson::SUCCESS) p.num(e, d.rise, pp + "/rise", -100.0, 100.0);
                if (element e; po["life"].get(e) == simdjson::SUCCESS) p.num(e, d.life, pp + "/life", 0.001, 600.0);
                fx.proxy = d;
            } else if (element e; o["proxy"].get(e) == simdjson::SUCCESS) {
                p.fail(path + "/proxy", "not an object");
            }
            s.effects.push_back(std::move(fx));
        }
    } else if (element e; doc["effects"].get(e) == simdjson::SUCCESS) {
        p.fail("/effects", "not an object of effects");
    }

    if (object decals; doc["decals"].get_object().get(decals) == simdjson::SUCCESS) {
        for (auto [name, value] : decals) {
            const std::string path = "/decals/" + esc(name);
            DecalDesc d;
            d.name = name;
            object o;
            if (value.get_object().get(o) != simdjson::SUCCESS) { p.fail(path, "not an object"); continue; }
            p.keys(o, {"texture", "size", "depth", "albedo", "roughness", "normal", "life"}, path);
            if (element e; o["texture"].get(e) == simdjson::SUCCESS) textureRef(e, d.texture, path + "/texture");
            if (element e; o["size"].get(e) == simdjson::SUCCESS) p.num(e, d.size, path + "/size", 0.001, 100.0);
            if (element e; o["depth"].get(e) == simdjson::SUCCESS) p.num(e, d.depth, path + "/depth", 0.001, 100.0);
            if (element e; o["albedo"].get(e) == simdjson::SUCCESS) p.color(e, d.albedo, path + "/albedo");
            if (element e; o["roughness"].get(e) == simdjson::SUCCESS) p.num(e, d.roughness, path + "/roughness", 0.0, 1.0);
            if (element e; o["normal"].get(e) == simdjson::SUCCESS) p.num(e, d.normal, path + "/normal", 0.0, 4.0);
            if (element e; o["life"].get(e) == simdjson::SUCCESS) p.num(e, d.life, path + "/life", 0.0, 3600.0);
            s.decals.push_back(std::move(d));
        }
    } else if (element e; doc["decals"].get(e) == simdjson::SUCCESS) {
        p.fail("/decals", "not an object of decals");
    }

    if (object tracers; doc["tracers"].get_object().get(tracers) == simdjson::SUCCESS) {
        for (auto [name, value] : tracers) {
            const std::string path = "/tracers/" + esc(name);
            TracerDesc t;
            t.name = name;
            object o;
            if (value.get_object().get(o) != simdjson::SUCCESS) { p.fail(path, "not an object"); continue; }
            p.keys(o, {"speed", "length", "width", "color", "intensity", "everyPlayer", "everyEnemy", "minDistance"}, path);
            if (element e; o["speed"].get(e) == simdjson::SUCCESS) p.num(e, t.speed, path + "/speed", 1.0, 10000.0);
            if (element e; o["length"].get(e) == simdjson::SUCCESS) p.num(e, t.length, path + "/length", 0.01, 100.0);
            if (element e; o["width"].get(e) == simdjson::SUCCESS) p.num(e, t.width, path + "/width", 0.0001, 1.0);
            if (element e; o["color"].get(e) == simdjson::SUCCESS) p.color(e, t.color, path + "/color");
            if (element e; o["intensity"].get(e) == simdjson::SUCCESS) p.num(e, t.intensity, path + "/intensity", 0.0, 10000.0);
            if (element e; o["everyPlayer"].get(e) == simdjson::SUCCESS) p.uint(e, t.everyPlayer, path + "/everyPlayer", 0, 1000);
            if (element e; o["everyEnemy"].get(e) == simdjson::SUCCESS) p.uint(e, t.everyEnemy, path + "/everyEnemy", 0, 1000);
            if (element e; o["minDistance"].get(e) == simdjson::SUCCESS) p.num(e, t.minDistance, path + "/minDistance", 0.0, 1000.0);
            s.tracers.push_back(std::move(t));
        }
    } else if (element e; doc["tracers"].get(e) == simdjson::SUCCESS) {
        p.fail("/tracers", "not an object of tracers");
    }

    // Events, by channel name: names resolved against what was declared above (a {tag} name through its
    // fallback).
    if (object events; doc["events"].get_object().get(events) == simdjson::SUCCESS) {
        for (auto [name, value] : events) {
            const std::string path = "/events/" + esc(name);
            s.events.emplace_back();
            EventBinding* b = &s.events.back();
            b->on = std::string(name);
            if (b->on.empty()) p.fail(path, "an event channel's name, e.g. slice.hit");
            object o;
            if (value.get_object().get(o) != simdjson::SUCCESS) { p.fail(path, "not an object"); continue; }
            p.keys(o, {"effect", "tracer", "decal", "fallback", "ahead"}, path);
            if (element e; o["fallback"].get(e) == simdjson::SUCCESS) p.str(e, b->fallback, path + "/fallback");
            if (element e; o["ahead"].get(e) == simdjson::SUCCESS) p.num(e, b->ahead, path + "/ahead", 0.0, 10.0);
            bool needsFallback = false;
            const auto check = [&](const std::string& n, const std::string& at, bool decal) {
                const bool surfaced = n.find("{tag}") != std::string::npos;
                needsFallback |= surfaced;
                const int i = decal ? resolveIn(s.decals, n, "", b->fallback) : resolveIn(s.effects, n, "", b->fallback);
                if (surfaced && b->fallback.empty()) return;   // reported once, at /fallback
                if (i < 0)
                    p.fail(at, std::string("no ") + (decal ? "decal" : "effect") + " named '" +
                                   (surfaced ? substitute(n, b->fallback) : n) + "'");
            };
            if (element e; o["effect"].get(e) == simdjson::SUCCESS) {
                array a;
                if (e.get_array().get(a) == simdjson::SUCCESS) {
                    std::size_t i = 0;
                    for (element n : a) {
                        std::string fx;
                        const std::string at = path + "/effect/" + std::to_string(i++);
                        if (p.str(n, fx, at)) {
                            check(fx, at, false);
                            b->effects.push_back(fx);
                        }
                    }
                } else if (std::string fx; p.str(e, fx, path + "/effect")) {
                    check(fx, path + "/effect", false);
                    b->effects.push_back(fx);
                }
            }
            if (element e; o["decal"].get(e) == simdjson::SUCCESS && p.str(e, b->decal, path + "/decal"))
                check(b->decal, path + "/decal", true);
            if (element e; o["tracer"].get(e) == simdjson::SUCCESS && p.str(e, b->tracer, path + "/tracer") &&
                           s.tracer(b->tracer) < 0)
                p.fail(path + "/tracer", "no tracer named '" + b->tracer + "'");
            if (needsFallback && b->fallback.empty()) p.fail(path + "/fallback", "a {tag} name needs a fallback tag");
        }
    } else if (element e; doc["events"].get(e) == simdjson::SUCCESS) {
        p.fail("/events", "not an object of events");
    }

    if (object caps; doc["caps"].get_object().get(caps) == simdjson::SUCCESS) {
        p.keys(caps, {"records", "particles", "decals", "tracers", "proxies"}, "/caps");
        if (element e; caps["records"].get(e) == simdjson::SUCCESS) p.uint(e, s.caps.records, "/caps/records", 1, 4096);
        if (element e; caps["particles"].get(e) == simdjson::SUCCESS) p.uint(e, s.caps.particles, "/caps/particles", 1, 1048576);
        if (element e; caps["decals"].get(e) == simdjson::SUCCESS) p.uint(e, s.caps.decals, "/caps/decals", 1, 4096);
        if (element e; caps["tracers"].get(e) == simdjson::SUCCESS) p.uint(e, s.caps.tracers, "/caps/tracers", 1, 1024);
        if (element e; caps["proxies"].get(e) == simdjson::SUCCESS) p.uint(e, s.caps.proxies, "/caps/proxies", 1, 256);
    } else if (element e; doc["caps"].get(e) == simdjson::SUCCESS) {
        p.fail("/caps", "not an object of caps");
    }

    if (!p.errors.empty()) return R::err(std::move(p.errors));
    return R::ok(std::move(s));
}

VfxSet defaultVfxSet() {
    // The old particle system's presets and combat_fx's dustFor (both deleted in step 2), as data (the
    // presets' spread angles are radians; the set's degrees). Dust and smoke blend as alpha and lit,
    // as the spec has them (the old path drew every particle additive).
    VfxSet s;
    const auto spark = [] {
        return emitterFrom(24, 1.2f, {2.0f, 8.0f}, {0.2f, 0.6f}, 0.03f, 0.005f, {1.0f, 0.9f, 0.5f}, 1.0f, {1.0f, 0.5f, 0.1f}, 0.0f,
                           9.81f, 0.5f, Blend::Additive, false);
    };
    const auto dust = [](glm::vec3 from, float alphaFrom, glm::vec3 to) {
        EmitterDesc d = emitterFrom(10, 0.7f, {0.4f, 1.6f}, {0.4f, 1.2f}, 0.05f, 0.25f, from, alphaFrom, to, 0.0f, 0.5f, 3.0f,
                                    Blend::Alpha, true);
        d.offset = 0.02f;
        return d;
    };
    EffectDesc muzzle{"muzzle", {emitterFrom(16, 0.3f, {8.0f, 15.0f}, {0.05f, 0.15f}, 0.05f, 0.01f, {1.0f, 0.8f, 0.3f}, 1.0f,
                                             {1.0f, 0.4f, 0.1f}, 0.0f, 0.0f, 5.0f, Blend::Additive, false)},
                      std::nullopt, std::nullopt};
    s.effects.push_back(muzzle);
    s.effects.push_back({"impact_concrete", {spark(), dust({0.62f, 0.6f, 0.56f}, 0.55f, {0.5f, 0.48f, 0.45f})}, std::nullopt,
                         std::nullopt});
    {
        EmitterDesc metal = spark();
        metal.count = 14;
        metal.speed = {4.0f, 11.0f};
        metal.color.keys.front().second = {1.0f, 0.95f, 0.75f};
        s.effects.push_back({"impact_metal", {spark(), metal}, std::nullopt, std::nullopt});
    }
    {
        EmitterDesc wood = dust({0.45f, 0.32f, 0.2f}, 0.6f, {0.35f, 0.25f, 0.15f});
        wood.gravity = 3.0f;
        wood.sizeStart = 0.03f;
        wood.sizeEnd = 0.08f;
        s.effects.push_back({"impact_wood", {spark(), wood}, std::nullopt, std::nullopt});
    }
    s.effects.push_back({"impact_default", {spark(), dust({0.7f, 0.7f, 0.7f}, 0.45f, {0.6f, 0.6f, 0.6f})}, std::nullopt,
                         std::nullopt});
    s.effects.push_back({"grenade_boom",
                         {emitterFrom(128, 3.14159f, {3.0f, 12.0f}, {0.5f, 2.0f}, 0.2f, 0.5f, {1.0f, 0.6f, 0.1f}, 1.0f,
                                      {0.3f, 0.1f, 0.05f}, 0.0f, 2.0f, 1.0f, Blend::Additive, false)},
                         std::nullopt, std::nullopt});
    s.effects.push_back({"smoke_cloud",
                         {emitterFrom(8, 0.4f, {0.5f, 2.0f}, {1.0f, 4.0f}, 0.1f, 0.5f, {0.5f, 0.5f, 0.5f}, 0.6f,
                                      {0.3f, 0.3f, 0.3f}, 0.0f, -0.5f, 2.0f, Blend::Alpha, true)},
                         std::nullopt, std::nullopt});
    DecalDesc mark;   // ecs/impact_marks.cpp's quad: 8 cm, dark, rough
    mark.name = "mark";
    s.decals.push_back(mark);
    return s;   // no events: a game's set binds its own channels
}

std::string vfxSetJsonSchema() {
    return R"json({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://ohao.dev/schema/vfx-set.json",
  "title": "OHAO effect set (.vfx.json)",
  "description": "docs/specs/2026-10-28-vfx-design.md, 'Data: the VFX set'. Cross-references (an emitter's or decal's texture, an event's effects, decal and tracer, a {tag} name's fallback) are checked by the engine's parser.",
  "type": "object",
  "additionalProperties": false,
  "$defs": {
    "range": {"oneOf": [{"type": "number"}, {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2}],
              "description": "A number, or [min, max] drawn per particle"},
    "color": {"oneOf": [{"type": "string", "pattern": "^#[0-9a-fA-F]{6}$"},
                        {"type": "array", "items": {"type": "number", "minimum": 0}, "minItems": 3, "maxItems": 3}],
              "description": "\"#rrggbb\" (sRGB) or [r, g, b] (linear)"},
    "names": {"oneOf": [{"type": "string"}, {"type": "array", "items": {"type": "string"}}]},
    "binding": {"type": "object", "additionalProperties": false,
                "properties": {"effect": {"$ref": "#/$defs/names", "description": "Effects; a name may hold {tag} (the event's tag: a hit's surface)"},
                               "tracer": {"type": "string", "description": "From the effects' place to where the event ended"},
                               "decal": {"type": "string", "description": "On what the event reached; may hold {tag}"},
                               "fallback": {"type": "string", "description": "The tag a {tag} name falls back to"},
                               "ahead": {"type": "number", "minimum": 0, "maximum": 10, "description": "m along the event's direction the effects start (a muzzle ahead of the eye)"}}}
  },
  "properties": {
    "textures": {"type": "object", "additionalProperties": {
      "type": "object", "additionalProperties": false, "required": ["file"],
      "properties": {"file": {"type": "string", "minLength": 1}, "normal": {"type": "string"},
                     "cols": {"type": "integer", "minimum": 1, "maximum": 64}, "rows": {"type": "integer", "minimum": 1, "maximum": 64}}}},
    "effects": {"type": "object", "additionalProperties": {
      "type": "object", "additionalProperties": false, "required": ["emitters"],
      "properties": {
        "emitters": {"type": "array", "minItems": 1, "items": {
          "type": "object", "additionalProperties": false,
          "properties": {
            "count": {"type": "integer", "minimum": 0, "maximum": 4096, "description": "Particles in the burst"},
            "rate": {"type": "number", "minimum": 0, "description": "Particles a second for a placed emitter"},
            "life": {"$ref": "#/$defs/range"}, "speed": {"$ref": "#/$defs/range"},
            "size": {"$ref": "#/$defs/range", "description": "[start, end] over life, or one size"},
            "spread": {"type": "number", "minimum": 0, "maximum": 180, "description": "Cone half angle (deg) about the spawn's -Z"},
            "gravity": {"type": "number", "description": "m/s^2 along -Y (negative rises)"},
            "drag": {"type": "number", "minimum": 0, "description": "1/s"},
            "offset": {"type": "number", "description": "m along the spawn's -Z it starts from"},
            "color": {"description": "A colour, a list of colours over life, or [t, colour] keys"},
            "alpha": {"description": "A number, a list over life, or [t, value] keys"},
            "intensity": {"type": "number", "minimum": 0},
            "blend": {"enum": ["additive", "alpha", "premultiplied"]},
            "lit": {"type": "boolean"}, "soft": {"type": "number", "minimum": 0, "description": "Depth fade (m)"},
            "texture": {"type": "string"},
            "frames": {"oneOf": [{"enum": ["life", "random"]}, {"type": "integer", "minimum": 0}]},
            "sort": {"enum": ["auto", "none", "particles"]}}}},
        "light": {"type": "object", "additionalProperties": false,
                  "properties": {"color": {"$ref": "#/$defs/color"}, "intensity": {"type": "number", "minimum": 0},
                                 "range": {"type": "number", "minimum": 0}, "time": {"type": "number", "minimum": 0}}},
        "proxy": {"type": "object", "additionalProperties": false,
                  "properties": {"spheres": {"type": "integer", "minimum": 1, "maximum": 8}, "radius": {"$ref": "#/$defs/range"},
                                 "density": {"type": "number", "minimum": 0}, "rise": {"type": "number"},
                                 "life": {"type": "number", "minimum": 0}}}}}},
    "decals": {"type": "object", "additionalProperties": {
      "type": "object", "additionalProperties": false,
      "properties": {"texture": {"type": "string"}, "size": {"type": "number", "minimum": 0}, "depth": {"type": "number", "minimum": 0},
                     "albedo": {"$ref": "#/$defs/color"}, "roughness": {"type": "number", "minimum": 0, "maximum": 1},
                     "normal": {"type": "number", "minimum": 0}, "life": {"type": "number", "minimum": 0}}}},
    "tracers": {"type": "object", "additionalProperties": {
      "type": "object", "additionalProperties": false,
      "properties": {"speed": {"type": "number", "minimum": 1}, "length": {"type": "number", "minimum": 0},
                     "width": {"type": "number", "minimum": 0}, "color": {"$ref": "#/$defs/color"},
                     "intensity": {"type": "number", "minimum": 0}, "everyPlayer": {"type": "integer", "minimum": 0},
                     "everyEnemy": {"type": "integer", "minimum": 0}, "minDistance": {"type": "number", "minimum": 0}}}},
    "events": {"type": "object", "description": "What a game's events spawn, by channel name (events.read's names, e.g. slice.hit)",
               "additionalProperties": {"$ref": "#/$defs/binding"}},
    "caps": {"type": "object", "additionalProperties": false,
             "properties": {"records": {"type": "integer", "minimum": 1}, "particles": {"type": "integer", "minimum": 1},
                            "decals": {"type": "integer", "minimum": 1}, "tracers": {"type": "integer", "minimum": 1},
                            "proxies": {"type": "integer", "minimum": 1}}}
  }
}
)json";
}

} // namespace ohao::vfx
