#pragma once
// Shared by the method implementations; not part of the public API.
#include "api/params.hpp"
#include "api/session.hpp"
#include "ecs/json_parse.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/snapshot.hpp"
#include "ecs/world_diff.hpp"
#include "reflect/decode.hpp"
#include "reflect/json_writer.hpp"

#include <ankerl/unordered_dense.h>

#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::api {

using reflect::DecodeContext;
using reflect::Diagnostic;
using reflect::FieldInfo;
using reflect::JsonDocument;
using reflect::JsonNode;
using reflect::JsonType;
using reflect::JsonWriter;
using reflect::TypeInfo;

struct Session::Impl {
    Impl(ecs::World& w, const reflect::TypeRegistry& t);

    ecs::World& world;
    const reflect::TypeRegistry& types;
    // The time model: the session's own (lockstep) until a host shares its timeline.
    Timeline ownTime;
    Timeline* time{&ownTime};
    uint64_t schemaHash{0};
    std::string catalogue;            // built on the first schema.get
    ecs::JsonParser paramsParser;     // the request's params
    ecs::JsonParser rollbackParser;   // the undo batch built when a batch fails
    ecs::JsonParser valueParser;      // field values re-read by deep field.set paths
    // Every component type with data (ComponentOps), by name and in registration order.
    ankerl::unordered_dense::map<std::string_view, const TypeInfo*> byName;
    std::vector<const TypeInfo*> withOps;
    std::vector<const TypeInfo*> saved;          // the subset written to files
    std::vector<std::string_view> savedNames;    // for "did you mean"
    std::map<std::string, ecs::Snapshot, std::less<>> snapshots;   // by name
    ecs::Snapshot hashScratch;                                     // world.hash's capture, reused
    ecs::WorldDiff diffScratch;                                    // world.diff's result, reused
    ecs::AssetResolver* assets{nullptr};                           // checkAssets
    RenderHandler render;                                          // render.capture
    HostKind host{HostKind::None};
    std::optional<PathPolicy> paths;                               // the sandbox, when serving
    const TypeInfo* tName{nullptr};
    const TypeInfo* tTransform{nullptr};
    const TypeInfo* tParent{nullptr};

    // The call in progress.
    const CallControl* control{nullptr};
    bool paramsFailed{false};   // the errors are about the params themselves (BadParams)
    bool cancelled{false};

    [[nodiscard]] bool cancelRequested() const noexcept {
        return control && control->cancel && control->cancel->load(std::memory_order_relaxed);
    }
};

// A method writes its result value into `out`, or reports into ctx.errors and returns false.
using MethodFn = bool (*)(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
                          JsonWriter& out);
bool methodApply(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx, JsonWriter& out);
bool methodQuery(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx, JsonWriter& out);
bool methodSummary(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
                   JsonWriter& out);

// ---- query planning (query.cpp), shared by world.query, world.step's watch and world.diff --

// One projected component: the whole value, or the listed fields.
struct Projection {
    const TypeInfo* type;
    bool whole{false};
    std::vector<const FieldInfo*> fields;
};
// Reads a "fields" array ("/Type" or "/Type/field"; ctx is positioned at its parent).
// False after reporting (E_PATH, E_COMPONENT_UNKNOWN).
bool parseProjection(Session::Impl& s, const JsonDocument& doc, const JsonNode& fields, DecodeContext& ctx,
                     std::vector<Projection>& out);

// A checked selection and projection. Its views point into the request's document.
struct QueryPlan {
    bool byIds{false};
    std::vector<uint64_t> ids;
    std::vector<std::string_view> names;
    std::string_view nameContains;
    std::vector<const ecs::ComponentOps*> has;
    bool projected{false};
    std::vector<Projection> projection;
    std::size_t limit{50};
    std::size_t offset{0};
    bool defaults{false};
};
// Reads the "ids", "names", "has" and "fields" members of `obj` (already type-checked by
// its params struct) into `plan`. False after reporting.
bool planQuery(Session::Impl& s, const JsonDocument& doc, const JsonNode& obj, DecodeContext& ctx, QueryPlan& plan);
// {entities: [...], count, missing?, next?}, matching entities in creation order.
void writeQuery(Session::Impl& s, const QueryPlan& plan, JsonWriter& out);

// ---- helpers (session.cpp) -------------------------------------------------------------

[[nodiscard]] std::string hex16(uint64_t id);
// Reports unknown members (E_UNKNOWN_FIELD, with a suggestion) and repeated ones
// (E_DUPLICATE_KEY). `obj` must be an object. `what` names it in messages.
bool checkKeys(const JsonDocument& doc, const JsonNode& obj, std::initializer_list<std::string_view> allowed,
               std::string_view what, DecodeContext& ctx);
// Optional members with a type check: absent leaves `out` alone; present but wrong
// reports (with the member's path) and returns false.
bool optBool(const JsonDocument& doc, const JsonNode& obj, std::string_view key, DecodeContext& ctx, bool& out);
bool optInt(const JsonDocument& doc, const JsonNode& obj, std::string_view key, DecodeContext& ctx, int64_t lo,
            int64_t hi, int64_t& out);
bool optString(const JsonDocument& doc, const JsonNode& obj, std::string_view key, DecodeContext& ctx,
               std::string_view& out);

// Marks the call's errors as being about its params (BadParams); returns false.
inline bool badParams(Session::Impl& s) {
    s.paramsFailed = true;
    return false;
}

// Decodes a method's params into its registered struct `P`: unknown members, types,
// enums and limits as scene files report them, then the required members. MCP's "_meta"
// is skipped. False (BadParams) after reporting.
template <class P>
bool decodeParams(Session::Impl& s, const JsonNode& params, DecodeContext& ctx, P& out) {
    const TypeInfo* t = paramsRegistry().find<P>();
    reflect::readStruct(&out, *t, params, ctx, "_meta");
    for (const auto& f : t->fields)
        if (f.attrs.required && params.type == JsonType::Object && !ctx.doc.member(params, f.name))
            ctx.error("E_COMMAND", params, t->doc + " need \"" + f.name + "\"", f.attrs.doc);
    return ctx.errors.empty() || badParams(s);
}

// The file a params path names: as given without a sandbox, else resolved by it (E_ACCESS
// when outside). `key` is the member holding it. False after reporting.
bool usePath(Session::Impl& s, const JsonNode& params, std::string_view key, std::string_view path, PathUse use,
             DecodeContext& ctx, std::string& resolved);
// A component type with data by name. Unknown names report E_COMPONENT_UNKNOWN with a
// suggestion; with `savedOnly`, so do runtime types. `at` is the node naming it.
[[nodiscard]] const TypeInfo* findComponent(Session::Impl& s, std::string_view name, const JsonNode& at,
                                            bool savedOnly, DecodeContext& ctx);
// The entity's saved components as "Name": {...} members of an open object, in file
// order and file form (fully default Name/Transform and default fields left out unless
// `includeDefaults`), followed by its preserved unknown components when `preserved`.
void writeComponents(Session::Impl& s, JsonWriter& w, entt::entity e, bool includeDefaults, bool preserved);

} // namespace ohao::api
