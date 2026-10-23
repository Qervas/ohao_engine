#pragma once
// Shared by the method implementations; not part of the public API.
#include "api/session.hpp"
#include "ecs/json_parse.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/snapshot.hpp"
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
    uint64_t frame{0};
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
    ecs::AssetResolver* assets{nullptr};                           // checkAssets
    RenderHandler render;                                          // render.capture
    const TypeInfo* tName{nullptr};
    const TypeInfo* tTransform{nullptr};
    const TypeInfo* tParent{nullptr};
};

// A method writes its result value into `out`, or reports into ctx.errors and returns false.
using MethodFn = bool (*)(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
                          JsonWriter& out);
bool methodApply(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx, JsonWriter& out);
bool methodQuery(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx, JsonWriter& out);
bool methodSummary(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
                   JsonWriter& out);

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
// A component type with data by name. Unknown names report E_COMPONENT_UNKNOWN with a
// suggestion; with `savedOnly`, so do runtime types. `at` is the node naming it.
[[nodiscard]] const TypeInfo* findComponent(Session::Impl& s, std::string_view name, const JsonNode& at,
                                            bool savedOnly, DecodeContext& ctx);
// The entity's saved components as "Name": {...} members of an open object, in file
// order and file form (fully default Name/Transform and default fields left out unless
// `includeDefaults`), followed by its preserved unknown components when `preserved`.
void writeComponents(Session::Impl& s, JsonWriter& w, entt::entity e, bool includeDefaults, bool preserved);

} // namespace ohao::api
