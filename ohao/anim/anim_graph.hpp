#pragma once
// Animation graphs (`.animgraph.json`, scene data) and their evaluation (sub-project 10,
// "The data model", "The step", "The frame"). ECS-free, so every rule is tested on its own.
//
// A graph names its skeleton (a glTF file and skin), its clips (a glTF file and animation each,
// looped or not, a rate, events), its parameters, and its layers. A layer has a joint mask (the
// named joints and every descendant; empty: all), a blend mode (replace or additive over the
// rest pose), a weight, an initial state, states (a clip, or a 1-D blend of clips by a
// parameter) and transitions (to a state, when a parameter compares, an event fired this step,
// or the current clip ended; with a fade in steps).
//
// The step (stepLayer) is integers only: the state, its steps, the state faded from, its steps,
// the fade's length and progress. A pose (evaluatePose) is a function of that state, the
// parameters and alpha, sampled at (steps - 1 + alpha) steps of kStepSeconds: alpha 1 is the
// newest step's pose, alpha 0 the one before.
#include "anim/skeleton.hpp"
#include "core/result.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::anim {

// Clip time advances by one fixed simulation step a step (the simulation is 60 Hz by ruling).
inline constexpr float kStepSeconds = 1.0f / 60.0f;

enum class ParamType : uint8_t { Float, Bool };
enum class LayerBlend : uint8_t { Replace, Additive };
enum class CompareOp : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };

struct GraphClip {
    std::string name, file, animation;
    bool loop = true;
    float rate = 1.0f;
    std::vector<ClipEvent> events;   // added to the glTF clip's (glTF has no event channel)
};

struct BlendPoint {
    std::string clip;
    float at = 0.0f;   // the parameter value at which this clip has full weight (ascending)
};

struct GraphState {
    std::string name;
    std::string clip;            // a single clip, or
    std::string blendParam;      // a 1-D blend by this parameter over `points`
    std::vector<BlendPoint> points;
    float rate = 1.0f;           // times the clip's own rate
    float startAt = 0.0f;        // seconds into the clip at entry
};

struct Condition {
    enum class Kind : uint8_t { Param, Event, ClipEnd } kind = Kind::ClipEnd;
    std::string name;   // the parameter or event
    CompareOp op = CompareOp::Eq;
    float value = 0.0f;
};

struct Transition {
    std::string from, to;
    Condition when;
    uint16_t fade = 0;   // steps
};

struct GraphLayer {
    std::string name;
    LayerBlend blend = LayerBlend::Replace;
    float weight = 1.0f;
    std::vector<std::string> mask;   // joint names (each with its descendants); empty: every joint
    std::string initial;
    std::vector<GraphState> states;
    std::vector<Transition> transitions;   // evaluated in order; the first that holds is taken
};

struct AnimGraph {
    std::string skeletonFile, skin;
    std::string driver;   // the code that writes the parameters ("" for none)
    std::vector<GraphClip> clips;
    std::vector<std::pair<std::string, ParamType>> params;
    std::vector<GraphLayer> layers;
};

struct GraphError {
    std::string path;   // JSON pointer into the graph file
    std::string message;
};

// The JSON format; structural errors only (types, missing members, unknown keys).
[[nodiscard]] Result<AnimGraph, std::vector<GraphError>> parseAnimGraph(std::string_view json);

// A graph checked against its skeleton and clips, with every name resolved to an index.
struct BoundState {
    int clip = -1;                 // single clip, or
    int blendParam = -1;           // a blend over
    std::vector<std::pair<int, float>> points;   // (clip index, at)
    float rate = 1.0f, startAt = 0.0f;
};
struct BoundTransition {
    int from = -1, to = -1;
    Condition::Kind kind = Condition::Kind::ClipEnd;
    int param = -1;
    std::string event;
    CompareOp op = CompareOp::Eq;
    float value = 0.0f;
    uint16_t fade = 0;
};
struct BoundLayer {
    std::string name;
    LayerBlend blend = LayerBlend::Replace;
    float weight = 1.0f;
    std::vector<uint8_t> mask;   // per joint, 1 = in; empty = every joint
    int initial = 0;
    std::vector<std::string> stateNames;
    std::vector<BoundState> states;
    std::vector<BoundTransition> transitions;
};
struct BoundClip {
    std::string name;
    Clip clip;     // bound to the skeleton; events = the glTF's and the graph's, sorted
    bool loop = true;
    float rate = 1.0f;
};
struct BoundGraph {
    AnimGraph source;
    Skeleton skeleton;
    std::vector<BoundClip> clips;
    std::vector<std::string> paramNames;
    std::vector<ParamType> paramTypes;
    std::vector<BoundLayer> layers;
    [[nodiscard]] int param(std::string_view name) const;
    [[nodiscard]] int state(int layer, std::string_view name) const;
};

// Binds and validates: a parameter, clip, state or mask joint that does not exist; a state no
// transition leaves (except a looping one); an event time outside its clip; a blend whose points
// are not ascending. Every problem is reported, each with its path.
[[nodiscard]] Result<BoundGraph, std::vector<GraphError>> bindGraph(AnimGraph graph, Skeleton skeleton,
                                                                    std::span<const Clip> fileClips);

// A layer's saved state (the ECS keeps it in the Animator component, names as written).
struct LayerState {
    int state = 0;
    uint32_t steps = 0;
    int from = -1;            // the state faded from, -1 for none
    uint32_t fromSteps = 0;
    uint16_t fade = 0, faded = 0;
};

// What the drivers set for this step: a value per graph parameter, and the events fired.
struct Params {
    std::vector<float> values;         // index = BoundGraph::param; missing = 0
    std::vector<std::string> events;   // names, any order
    [[nodiscard]] float get(int index) const { return index >= 0 && index < static_cast<int>(values.size()) ? values[index] : 0.0f; }
    [[nodiscard]] bool fired(std::string_view name) const;
};

struct FiredEvent {
    int layer = 0;
    int clip = -1;
    std::string name;
};

// Initial layer states (each layer at its initial state, step 0).
[[nodiscard]] std::vector<LayerState> initialLayers(const BoundGraph& graph);

// One step of one layer:
//   1. a fade that completed on the previous step ends (from = -1);
//   2. the first transition out of the current state that holds is taken (the new state at step
//      0, the old one kept as `from` with its steps when the fade is > 0);
//   3. steps (and fromSteps, faded while fading) advance by one;
//   4. the current state's clip events whose time falls in (previous time, new time] fire, wraps
//      of a looping clip included, in time order (a blend state fires its heaviest clip's).
void stepLayer(const BoundGraph& graph, int layer, LayerState& state, const Params& params,
               std::vector<FiredEvent>& fired);

// The seconds into a state's clip at `steps` (fractional) steps since entry, before wrapping.
[[nodiscard]] float stateTime(const BoundGraph& graph, const BoundState& state, int clip, float steps);

// Whether a non-looping state's clip has reached its end at `steps` steps.
[[nodiscard]] bool clipEnded(const BoundGraph& graph, int layer, const LayerState& state);

struct PoseScratch {
    LocalPose rest, layer, partner, blendTmp;
    std::vector<glm::mat4> model;
};

// The local pose of every layer's state at (steps - 1 + alpha) steps (0 when steps is 0), its
// fade partner blended in by (faded - 1 + alpha) / fade, the layers combined in order.
void evaluatePose(const BoundGraph& graph, std::span<const LayerState> layers, const Params& params, float alpha,
                  LocalPose& out, PoseScratch& scratch);

} // namespace ohao::anim
