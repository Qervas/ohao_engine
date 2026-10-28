#include "anim/anim_graph.hpp"

#include <simdjson.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace ohao::anim {

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
    std::vector<GraphError> errors;
    void fail(std::string path, std::string message) { errors.push_back({std::move(path), std::move(message)}); }

    bool str(element e, std::string& out, const std::string& path) {
        std::string_view s;
        if (e.get_string().get(s) != simdjson::SUCCESS) { fail(path, "not a string"); return false; }
        out = s;
        return true;
    }
    bool num(element e, float& out, const std::string& path) {
        double d;
        if (e.get_double().get(d) != simdjson::SUCCESS) {
            bool b;
            if (e.get_bool().get(b) == simdjson::SUCCESS) { out = b ? 1.0f : 0.0f; return true; }
            fail(path, "not a number");
            return false;
        }
        out = static_cast<float>(d);
        return true;
    }
    // Every key of `o` must be one of `known`.
    void keys(object o, std::initializer_list<std::string_view> known, const std::string& path) {
        for (auto [k, v] : o) {
            (void)v;
            if (std::find(known.begin(), known.end(), k) == known.end()) fail(path + "/" + esc(k), "unknown key");
        }
    }
};

bool compare(float a, CompareOp op, float b) {
    switch (op) {
    case CompareOp::Eq: return a == b;
    case CompareOp::Ne: return a != b;
    case CompareOp::Lt: return a < b;
    case CompareOp::Le: return a <= b;
    case CompareOp::Gt: return a > b;
    case CompareOp::Ge: return a >= b;
    }
    return false;
}

const BoundClip* clipOf(const BoundGraph& g, int clip) {
    return clip >= 0 && clip < static_cast<int>(g.clips.size()) ? &g.clips[static_cast<std::size_t>(clip)] : nullptr;
}

// The clip that times a state (a blend: its first point's) and, for events, the heaviest at `param`.
int timingClip(const BoundState& s) { return s.clip >= 0 ? s.clip : (s.points.empty() ? -1 : s.points.front().first); }
int heaviestClip(const BoundState& s, const Params& p) {
    if (s.clip >= 0 || s.points.empty()) return s.clip;
    const float v = p.get(s.blendParam);
    int best = s.points.front().first;
    float bestDistance = std::abs(v - s.points.front().second);
    for (const auto& [clip, at] : s.points)
        if (std::abs(v - at) < bestDistance) best = clip, bestDistance = std::abs(v - at);
    return best;
}

} // namespace

bool Params::fired(std::string_view name) const { return std::find(events.begin(), events.end(), name) != events.end(); }

int BoundGraph::param(std::string_view name) const {
    for (std::size_t i = 0; i < paramNames.size(); ++i)
        if (paramNames[i] == name) return static_cast<int>(i);
    return -1;
}

int BoundGraph::state(int layer, std::string_view name) const {
    if (layer < 0 || layer >= static_cast<int>(layers.size())) return -1;
    const auto& names = layers[static_cast<std::size_t>(layer)].stateNames;
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return static_cast<int>(i);
    return -1;
}

Result<AnimGraph, std::vector<GraphError>> parseAnimGraph(std::string_view json) {
    using R = Result<AnimGraph, std::vector<GraphError>>;
    simdjson::dom::parser parser;
    const simdjson::padded_string padded(json);
    element root;
    if (const auto err = parser.parse(padded).get(root); err != simdjson::SUCCESS)
        return R::err({{"", std::string("not JSON: ") + simdjson::error_message(err)}});
    object doc;
    if (root.get_object().get(doc) != simdjson::SUCCESS) return R::err({{"", "the graph is not an object"}});
    Parser p;
    AnimGraph g;
    p.keys(doc, {"skeleton", "driver", "clips", "params", "layers"}, "");

    object skeleton;
    if (doc["skeleton"].get_object().get(skeleton) != simdjson::SUCCESS) p.fail("/skeleton", "missing: {file, skin}");
    else {
        p.keys(skeleton, {"file", "skin"}, "/skeleton");
        element e;
        if (skeleton["file"].get(e) != simdjson::SUCCESS) p.fail("/skeleton/file", "missing");
        else p.str(e, g.skeletonFile, "/skeleton/file");
        if (skeleton["skin"].get(e) == simdjson::SUCCESS) p.str(e, g.skin, "/skeleton/skin");
    }
    if (element e; doc["driver"].get(e) == simdjson::SUCCESS) p.str(e, g.driver, "/driver");

    object clips;
    if (doc["clips"].get_object().get(clips) != simdjson::SUCCESS) p.fail("/clips", "missing: an object of clips");
    else
        for (auto [name, value] : clips) {
            const std::string path = "/clips/" + esc(name);
            GraphClip c;
            c.name = name;
            object o;
            if (value.get_object().get(o) != simdjson::SUCCESS) { p.fail(path, "not an object"); continue; }
            p.keys(o, {"file", "animation", "loop", "rate", "rateFrom", "events"}, path);
            element e;
            if (o["file"].get(e) != simdjson::SUCCESS) p.fail(path + "/file", "missing");
            else p.str(e, c.file, path + "/file");
            if (o["animation"].get(e) != simdjson::SUCCESS) p.fail(path + "/animation", "missing");
            else p.str(e, c.animation, path + "/animation");
            if (o["loop"].get(e) == simdjson::SUCCESS && e.get_bool().get(c.loop) != simdjson::SUCCESS) p.fail(path + "/loop", "not a boolean");
            const bool rate = o["rate"].get(e) == simdjson::SUCCESS;
            if (rate) p.num(e, c.rate, path + "/rate");
            if (o["rateFrom"].get(e) == simdjson::SUCCESS) {
                object rf;
                if (rate) p.fail(path + "/rateFrom", "rate and rateFrom: one or the other");
                if (e.get_object().get(rf) != simdjson::SUCCESS) {
                    p.fail(path + "/rateFrom", "not an object: {param, at}");
                } else {
                    p.keys(rf, {"param", "at"}, path + "/rateFrom");
                    element x;
                    if (rf["param"].get(x) != simdjson::SUCCESS) p.fail(path + "/rateFrom/param", "missing");
                    else p.str(x, c.rateParam, path + "/rateFrom/param");
                    if (rf["at"].get(x) == simdjson::SUCCESS) p.str(x, c.rateAt, path + "/rateFrom/at");
                }
            }
            array events;
            if (o["events"].get_array().get(events) == simdjson::SUCCESS) {
                std::size_t i = 0;
                for (element ev : events) {
                    const std::string ep = path + "/events/" + std::to_string(i++);
                    object eo;
                    if (ev.get_object().get(eo) != simdjson::SUCCESS) { p.fail(ep, "not an object"); continue; }
                    p.keys(eo, {"t", "name"}, ep);
                    ClipEvent ce;
                    element x;
                    if (eo["t"].get(x) != simdjson::SUCCESS) p.fail(ep + "/t", "missing");
                    else p.num(x, ce.t, ep + "/t");
                    if (eo["name"].get(x) != simdjson::SUCCESS) p.fail(ep + "/name", "missing");
                    else p.str(x, ce.name, ep + "/name");
                    c.events.push_back(std::move(ce));
                }
            }
            g.clips.push_back(std::move(c));
        }

    object params;
    if (doc["params"].get_object().get(params) == simdjson::SUCCESS)
        for (auto [name, value] : params) {
            std::string_view type;
            if (value.get_string().get(type) != simdjson::SUCCESS || (type != "float" && type != "bool"))
                p.fail("/params/" + esc(name), "not \"float\" or \"bool\"");
            g.params.emplace_back(std::string(name), type == "bool" ? ParamType::Bool : ParamType::Float);
        }

    array layers;
    if (doc["layers"].get_array().get(layers) != simdjson::SUCCESS || layers.size() == 0)
        p.fail("/layers", "missing: an array of at least one layer");
    else {
        std::size_t li = 0;
        for (element le : layers) {
            const std::string lp = "/layers/" + std::to_string(li++);
            GraphLayer layer;
            object lo;
            if (le.get_object().get(lo) != simdjson::SUCCESS) { p.fail(lp, "not an object"); continue; }
            p.keys(lo, {"name", "blend", "weight", "mask", "initial", "states", "transitions"}, lp);
            element e;
            if (lo["name"].get(e) == simdjson::SUCCESS) p.str(e, layer.name, lp + "/name");
            if (lo["blend"].get(e) == simdjson::SUCCESS) {
                std::string b;
                if (p.str(e, b, lp + "/blend")) {
                    if (b == "replace") layer.blend = LayerBlend::Replace;
                    else if (b == "additive") layer.blend = LayerBlend::Additive;
                    else p.fail(lp + "/blend", "not \"replace\" or \"additive\"");
                }
            }
            if (lo["weight"].get(e) == simdjson::SUCCESS) p.num(e, layer.weight, lp + "/weight");
            array mask;
            if (lo["mask"].get_array().get(mask) == simdjson::SUCCESS) {
                std::size_t k = 0;
                for (element m : mask) {
                    std::string j;
                    if (p.str(m, j, lp + "/mask/" + std::to_string(k))) layer.mask.push_back(std::move(j));
                    ++k;
                }
            }
            if (lo["initial"].get(e) != simdjson::SUCCESS) p.fail(lp + "/initial", "missing");
            else p.str(e, layer.initial, lp + "/initial");
            object states;
            if (lo["states"].get_object().get(states) != simdjson::SUCCESS) p.fail(lp + "/states", "missing: an object of states");
            else
                for (auto [name, value] : states) {
                    const std::string sp = lp + "/states/" + esc(name);
                    GraphState s;
                    s.name = name;
                    object so;
                    if (value.get_object().get(so) != simdjson::SUCCESS) { p.fail(sp, "not an object"); continue; }
                    p.keys(so, {"clip", "blend1d", "rate", "startAt"}, sp);
                    if (so["clip"].get(e) == simdjson::SUCCESS) p.str(e, s.clip, sp + "/clip");
                    object blend;
                    if (so["blend1d"].get_object().get(blend) == simdjson::SUCCESS) {
                        p.keys(blend, {"param", "points"}, sp + "/blend1d");
                        if (blend["param"].get(e) != simdjson::SUCCESS) p.fail(sp + "/blend1d/param", "missing");
                        else p.str(e, s.blendParam, sp + "/blend1d/param");
                        array points;
                        if (blend["points"].get_array().get(points) != simdjson::SUCCESS || points.size() < 2)
                            p.fail(sp + "/blend1d/points", "missing: at least two {clip, at}");
                        else {
                            std::size_t k = 0;
                            for (element pt : points) {
                                const std::string pp = sp + "/blend1d/points/" + std::to_string(k++);
                                object po;
                                if (pt.get_object().get(po) != simdjson::SUCCESS) { p.fail(pp, "not an object"); continue; }
                                p.keys(po, {"clip", "at"}, pp);
                                BlendPoint bp;
                                element x;
                                if (po["clip"].get(x) != simdjson::SUCCESS) p.fail(pp + "/clip", "missing");
                                else p.str(x, bp.clip, pp + "/clip");
                                if (po["at"].get(x) != simdjson::SUCCESS) p.fail(pp + "/at", "missing");
                                else p.num(x, bp.at, pp + "/at");
                                s.points.push_back(std::move(bp));
                            }
                        }
                    }
                    if (s.clip.empty() == s.blendParam.empty()) p.fail(sp, "needs exactly one of clip and blend1d");
                    if (so["rate"].get(e) == simdjson::SUCCESS) p.num(e, s.rate, sp + "/rate");
                    if (so["startAt"].get(e) == simdjson::SUCCESS) p.num(e, s.startAt, sp + "/startAt");
                    layer.states.push_back(std::move(s));
                }
            array transitions;
            if (lo["transitions"].get_array().get(transitions) == simdjson::SUCCESS) {
                std::size_t k = 0;
                for (element te : transitions) {
                    const std::string tp = lp + "/transitions/" + std::to_string(k++);
                    object to;
                    if (te.get_object().get(to) != simdjson::SUCCESS) { p.fail(tp, "not an object"); continue; }
                    p.keys(to, {"from", "to", "when", "fade"}, tp);
                    Transition t;
                    if (to["from"].get(e) != simdjson::SUCCESS) p.fail(tp + "/from", "missing");
                    else p.str(e, t.from, tp + "/from");
                    if (to["to"].get(e) != simdjson::SUCCESS) p.fail(tp + "/to", "missing");
                    else p.str(e, t.to, tp + "/to");
                    if (to["fade"].get(e) == simdjson::SUCCESS) {
                        int64_t f;
                        if (e.get_int64().get(f) != simdjson::SUCCESS || f < 0 || f > 600) p.fail(tp + "/fade", "not 0..600 steps");
                        else t.fade = static_cast<uint16_t>(f);
                    }
                    object when;
                    if (to["when"].get_object().get(when) != simdjson::SUCCESS) p.fail(tp + "/when", "missing");
                    else {
                        p.keys(when, {"param", "op", "value", "event", "clipEnd"}, tp + "/when");
                        if (when["param"].get(e) == simdjson::SUCCESS) {
                            t.when.kind = Condition::Kind::Param;
                            p.str(e, t.when.name, tp + "/when/param");
                            std::string op = "==";
                            if (when["op"].get(e) == simdjson::SUCCESS) p.str(e, op, tp + "/when/op");
                            if (op == "==") t.when.op = CompareOp::Eq;
                            else if (op == "!=") t.when.op = CompareOp::Ne;
                            else if (op == "<") t.when.op = CompareOp::Lt;
                            else if (op == "<=") t.when.op = CompareOp::Le;
                            else if (op == ">") t.when.op = CompareOp::Gt;
                            else if (op == ">=") t.when.op = CompareOp::Ge;
                            else p.fail(tp + "/when/op", "not one of == != < <= > >=");
                            if (when["value"].get(e) != simdjson::SUCCESS) p.fail(tp + "/when/value", "missing");
                            else p.num(e, t.when.value, tp + "/when/value");
                        } else if (when["event"].get(e) == simdjson::SUCCESS) {
                            t.when.kind = Condition::Kind::Event;
                            p.str(e, t.when.name, tp + "/when/event");
                        } else if (when["clipEnd"].get(e) == simdjson::SUCCESS) {
                            t.when.kind = Condition::Kind::ClipEnd;
                        } else {
                            p.fail(tp + "/when", "needs param, event or clipEnd");
                        }
                    }
                    layer.transitions.push_back(std::move(t));
                }
            }
            g.layers.push_back(std::move(layer));
        }
    }
    if (!p.errors.empty()) return R::err(std::move(p.errors));
    return R::ok(std::move(g));
}

Result<BoundGraph, std::vector<GraphError>> bindGraph(AnimGraph graph, Skeleton skeleton, std::span<const Clip> fileClips) {
    using R = Result<BoundGraph, std::vector<GraphError>>;
    std::vector<GraphError> errors;
    BoundGraph b;
    b.skeleton = std::move(skeleton);
    const auto clipIndex = [&](std::string_view name) {
        for (std::size_t i = 0; i < graph.clips.size(); ++i)
            if (graph.clips[i].name == name) return static_cast<int>(i);
        return -1;
    };

    for (const GraphClip& gc : graph.clips) {
        const std::string path = "/clips/" + esc(gc.name);
        BoundClip bc;
        bc.name = gc.name;
        bc.loop = gc.loop;
        bc.rate = gc.rate;
        const Clip* found = nullptr;
        for (const Clip& c : fileClips)
            if (c.name == gc.animation) { found = &c; break; }
        if (!found) {
            errors.push_back({path + "/animation", "no animation '" + gc.animation + "' in " + gc.file});
        } else {
            bc.clip = bindClip(*found, b.skeleton);
            bc.clip.name = gc.name;
        }
        for (std::size_t i = 0; i < gc.events.size(); ++i) {
            const ClipEvent& ev = gc.events[i];
            if (found && (ev.t < 0.0f || ev.t > bc.clip.duration))
                errors.push_back({path + "/events/" + std::to_string(i) + "/t",
                                  "time " + std::to_string(ev.t) + " s is outside the clip (0.." + std::to_string(bc.clip.duration) + " s)"});
            bc.clip.events.push_back(ev);
        }
        std::stable_sort(bc.clip.events.begin(), bc.clip.events.end(), [](const ClipEvent& x, const ClipEvent& y) { return x.t < y.t; });
        if (gc.rate < 0.0f) errors.push_back({path + "/rate", "negative"});
        b.clips.push_back(std::move(bc));
    }
    for (const auto& [name, type] : graph.params) {
        b.paramNames.push_back(name);
        b.paramTypes.push_back(type);
    }
    // rateFrom: a float parameter, and an anchor (the `at` event, else the clip's end) after 0.
    for (std::size_t i = 0; i < graph.clips.size(); ++i) {
        const GraphClip& gc = graph.clips[i];
        if (gc.rateParam.empty()) continue;
        const std::string path = "/clips/" + esc(gc.name) + "/rateFrom";
        BoundClip& bc = b.clips[i];
        bc.rateParam = b.param(gc.rateParam);
        if (bc.rateParam < 0)
            errors.push_back({path + "/param", "unknown parameter '" + gc.rateParam + "'"});
        else if (b.paramTypes[static_cast<std::size_t>(bc.rateParam)] != ParamType::Float)
            errors.push_back({path + "/param", "'" + gc.rateParam + "' is not a float"});
        bc.rateAnchor = bc.clip.duration;
        if (!gc.rateAt.empty()) {
            const auto ev = std::find_if(bc.clip.events.begin(), bc.clip.events.end(),
                                         [&](const ClipEvent& e) { return e.name == gc.rateAt; });
            if (ev == bc.clip.events.end()) errors.push_back({path + "/at", "no event '" + gc.rateAt + "' in the clip"});
            else bc.rateAnchor = ev->t;
        }
        if (!(bc.rateAnchor > 0.0f)) errors.push_back({path, "the anchor is at 0 s: nothing to scale"});
    }

    for (std::size_t li = 0; li < graph.layers.size(); ++li) {
        const GraphLayer& gl = graph.layers[li];
        const std::string lp = "/layers/" + std::to_string(li);
        BoundLayer bl;
        bl.name = gl.name;
        bl.blend = gl.blend;
        bl.weight = gl.weight;
        if (!gl.mask.empty()) {
            bl.mask.assign(b.skeleton.joints.size(), 0);
            for (std::size_t k = 0; k < gl.mask.size(); ++k) {
                const int j = b.skeleton.find(gl.mask[k]);
                if (j < 0) {
                    errors.push_back({lp + "/mask/" + std::to_string(k), "'" + gl.mask[k] + "' is not a joint of skin '" + b.skeleton.name + "'"});
                    continue;
                }
                bl.mask[static_cast<std::size_t>(j)] = 1;
            }
            for (std::size_t j = 0; j < b.skeleton.joints.size(); ++j)   // descendants (parents come first)
                if (const int parent = b.skeleton.joints[j].parent; parent >= 0 && bl.mask[static_cast<std::size_t>(parent)]) bl.mask[j] = 1;
        }
        for (const GraphState& gs : gl.states) {
            const std::string sp = lp + "/states/" + esc(gs.name);
            BoundState bs;
            bs.rate = gs.rate;
            bs.startAt = gs.startAt;
            if (!gs.clip.empty()) {
                bs.clip = clipIndex(gs.clip);
                if (bs.clip < 0) errors.push_back({sp + "/clip", "unknown clip '" + gs.clip + "'"});
            } else {
                bs.blendParam = b.param(gs.blendParam);
                if (bs.blendParam < 0) errors.push_back({sp + "/blend1d/param", "unknown parameter '" + gs.blendParam + "'"});
                for (std::size_t k = 0; k < gs.points.size(); ++k) {
                    const int c = clipIndex(gs.points[k].clip);
                    if (c < 0) errors.push_back({sp + "/blend1d/points/" + std::to_string(k) + "/clip", "unknown clip '" + gs.points[k].clip + "'"});
                    if (k > 0 && gs.points[k].at <= gs.points[k - 1].at)
                        errors.push_back({sp + "/blend1d/points/" + std::to_string(k) + "/at", "not ascending"});
                    bs.points.emplace_back(c, gs.points[k].at);
                }
            }
            bl.stateNames.push_back(gs.name);
            bl.states.push_back(std::move(bs));
        }
        const auto stateIndex = [&](std::string_view name) {
            for (std::size_t i = 0; i < bl.stateNames.size(); ++i)
                if (bl.stateNames[i] == name) return static_cast<int>(i);
            return -1;
        };
        bl.initial = stateIndex(gl.initial);
        if (bl.initial < 0) {
            errors.push_back({lp + "/initial", "unknown state '" + gl.initial + "'"});
            bl.initial = 0;
        }
        for (std::size_t k = 0; k < gl.transitions.size(); ++k) {
            const Transition& t = gl.transitions[k];
            const std::string tp = lp + "/transitions/" + std::to_string(k);
            BoundTransition bt;
            bt.from = stateIndex(t.from);
            bt.to = stateIndex(t.to);
            if (bt.from < 0) errors.push_back({tp + "/from", "unknown state '" + t.from + "'"});
            if (bt.to < 0) errors.push_back({tp + "/to", "unknown state '" + t.to + "'"});
            bt.kind = t.when.kind;
            bt.op = t.when.op;
            bt.value = t.when.value;
            bt.fade = t.fade;
            if (t.when.kind == Condition::Kind::Param) {
                bt.param = b.param(t.when.name);
                if (bt.param < 0) errors.push_back({tp + "/when/param", "unknown parameter '" + t.when.name + "'"});
            } else if (t.when.kind == Condition::Kind::Event) {
                bt.event = t.when.name;
            }
            bl.transitions.push_back(std::move(bt));
        }
        // A state that ends (a one-shot clip) must have a way out.
        for (std::size_t s = 0; s < bl.states.size(); ++s) {
            const int clip = timingClip(bl.states[s]);
            const bool oneShot = clip >= 0 && !b.clips[static_cast<std::size_t>(clip)].loop && bl.states[s].rate > 0.0f;
            const bool leaves = std::any_of(bl.transitions.begin(), bl.transitions.end(),
                                            [&](const BoundTransition& t) { return t.from == static_cast<int>(s); });
            if (oneShot && !leaves)
                errors.push_back({lp + "/states/" + esc(bl.stateNames[s]), "a one-shot clip with no way out (add a transition from it)"});
        }
        b.layers.push_back(std::move(bl));
    }
    b.source = std::move(graph);
    if (!errors.empty()) return R::err(std::move(errors));
    return R::ok(std::move(b));
}

std::vector<LayerState> initialLayers(const BoundGraph& graph) {
    std::vector<LayerState> out(graph.layers.size());
    for (std::size_t i = 0; i < out.size(); ++i) out[i].state = graph.layers[i].initial;
    return out;
}

float stateTime(const BoundGraph& graph, const BoundState& state, int clip, float steps, const Params& params) {
    const BoundClip* c = clipOf(graph, clip);
    // rateFrom: the anchor lands `len` seconds in, as a fraction of len so that steps * dt == len
    // reaches the anchor exactly.
    if (c && c->rateParam >= 0)
        if (const float len = params.get(c->rateParam); len > 0.0f)
            return state.startAt + c->rateAnchor * state.rate * (steps * kStepSeconds / len);
    const float rate = state.rate * (c ? c->rate : 1.0f);
    return state.startAt + steps * kStepSeconds * rate;
}

bool clipEnded(const BoundGraph& graph, int layer, const LayerState& state, const Params& params) {
    const BoundLayer& l = graph.layers[static_cast<std::size_t>(layer)];
    if (state.state < 0 || state.state >= static_cast<int>(l.states.size())) return false;
    const BoundState& s = l.states[static_cast<std::size_t>(state.state)];
    const int clip = timingClip(s);
    const BoundClip* c = clipOf(graph, clip);
    if (!c || c->loop) return false;
    return stateTime(graph, s, clip, static_cast<float>(state.steps), params) >= c->clip.duration;
}

void stepLayer(const BoundGraph& graph, int layer, LayerState& state, const Params& params, std::vector<FiredEvent>& fired) {
    const BoundLayer& l = graph.layers[static_cast<std::size_t>(layer)];
    // 1. A fade that completed on the previous step ends.
    if (state.fade > 0 && state.faded >= state.fade) {
        state.from = -1;
        state.fromSteps = 0;
        state.fade = state.faded = 0;
    }
    // 2. The first transition out of the current state that holds.
    for (const BoundTransition& t : l.transitions) {
        if (t.from != state.state || t.to < 0) continue;
        bool holds = false;
        switch (t.kind) {
        case Condition::Kind::Param: holds = compare(params.get(t.param), t.op, t.value); break;
        case Condition::Kind::Event: holds = params.fired(t.event); break;
        case Condition::Kind::ClipEnd: holds = clipEnded(graph, layer, state, params); break;
        }
        if (!holds) continue;
        if (t.fade > 0) {
            state.from = state.state;
            state.fromSteps = state.steps;
            state.fade = t.fade;
            state.faded = 0;
        } else {
            state.from = -1;
            state.fromSteps = 0;
            state.fade = state.faded = 0;
        }
        state.state = t.to;
        state.steps = 0;
        break;
    }
    // 3. Advance.
    ++state.steps;
    if (state.from >= 0) ++state.fromSteps;
    if (state.fade > 0 && state.faded < state.fade) ++state.faded;
    // 4. The clip events crossed this step, in time order.
    if (state.state < 0 || state.state >= static_cast<int>(l.states.size())) return;
    const BoundState& s = l.states[static_cast<std::size_t>(state.state)];
    const int clip = heaviestClip(s, params);
    const BoundClip* c = clipOf(graph, clip);
    if (!c || c->clip.events.empty()) return;
    const float prev = stateTime(graph, s, clip, static_cast<float>(state.steps - 1), params);
    const float cur = stateTime(graph, s, clip, static_cast<float>(state.steps), params);
    if (!(cur > prev)) return;
    const float d = c->clip.duration;
    std::vector<std::pair<float, const std::string*>> hits;
    if (c->loop && d > 0.0f) {
        const float k0 = std::floor(prev / d), k1 = std::floor(cur / d);
        for (float k = k0; k <= k1; k += 1.0f)
            for (const ClipEvent& e : c->clip.events) {
                const float at = e.t + k * d;
                if (at > prev && at <= cur) hits.emplace_back(at, &e.name);
            }
    } else {
        for (const ClipEvent& e : c->clip.events)
            if (e.t > prev && e.t <= cur) hits.emplace_back(e.t, &e.name);
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    for (const auto& [at, name] : hits) fired.push_back(FiredEvent{layer, clip, *name});
}

namespace {

// A state's local pose at `steps` (fractional) steps, over the rest pose.
void sampleState(const BoundGraph& g, const BoundState& s, const Params& params, float steps, LocalPose& out, LocalPose& tmp) {
    const auto one = [&](int clip, LocalPose& into) {
        const BoundClip* c = clipOf(g, clip);
        if (!c) return;
        const float t = clipTime(c->clip.duration, stateTime(g, s, clip, steps, params), c->loop);
        sampleClip(c->clip, t, into);
    };
    restPose(g.skeleton, out);
    if (s.clip >= 0) { one(s.clip, out); return; }
    if (s.points.empty()) return;
    const float v = params.get(s.blendParam);
    if (v <= s.points.front().second) { one(s.points.front().first, out); return; }
    if (v >= s.points.back().second) { one(s.points.back().first, out); return; }
    std::size_t k = 1;
    while (k < s.points.size() && s.points[k].second < v) ++k;
    const auto& [c0, a0] = s.points[k - 1];
    const auto& [c1, a1] = s.points[k];
    one(c0, out);
    restPose(g.skeleton, tmp);
    one(c1, tmp);
    blendPose(out, tmp, (v - a0) / (a1 - a0), {});
}

} // namespace

void evaluatePose(const BoundGraph& graph, std::span<const LayerState> layers, const Params& params, float alpha,
                  LocalPose& out, PoseScratch& scratch) {
    restPose(graph.skeleton, scratch.rest);
    out = scratch.rest;
    for (std::size_t li = 0; li < graph.layers.size() && li < layers.size(); ++li) {
        const BoundLayer& l = graph.layers[li];
        const LayerState& st = layers[li];
        if (st.state < 0 || st.state >= static_cast<int>(l.states.size())) continue;
        const float steps = std::max(0.0f, static_cast<float>(st.steps) - 1.0f + alpha);
        sampleState(graph, l.states[static_cast<std::size_t>(st.state)], params, steps, scratch.layer, scratch.blendTmp);
        if (st.from >= 0 && st.from < static_cast<int>(l.states.size()) && st.fade > 0) {
            const float fromSteps = std::max(0.0f, static_cast<float>(st.fromSteps) - 1.0f + alpha);
            sampleState(graph, l.states[static_cast<std::size_t>(st.from)], params, fromSteps, scratch.partner, scratch.blendTmp);
            const float w = std::clamp((static_cast<float>(st.faded) - 1.0f + alpha) / static_cast<float>(st.fade), 0.0f, 1.0f);
            blendPose(scratch.partner, scratch.layer, w, {});
            std::swap(scratch.layer, scratch.partner);
        }
        if (l.blend == LayerBlend::Replace) blendPose(out, scratch.layer, l.weight, l.mask);
        else addPose(out, scratch.layer, scratch.rest, l.weight, l.mask);
    }
}

} // namespace ohao::anim
