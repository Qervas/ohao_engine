#pragma once
// Scene files (*.scene.json): canonical save, two-phase load, validation without a World.
//
// Save writes every live entity in creation order: "id" first, then each saved component
// in registration order, then preserved unknown components by name. The same world
// always gives the same bytes.
//
// Load is two-phase. Phase 1 parses and decodes the whole document and collects every
// error (up to 100). Only when it finds none does phase 2 create the entities, in file
// order, so a failed load never touches the destination world.
#include "core/result.hpp"
#include "ecs/world.hpp"
#include "reflect/decode.hpp"
#include "reflect/json_writer.hpp"
#include "reflect/registry.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::ecs {

using reflect::Diagnostic;

struct SaveOptions {
    reflect::JsonLayout layout{reflect::JsonLayout::Pretty};   // Pretty ends with '\n'
};

struct SaveReport {
    std::vector<Diagnostic> warnings;
};

// One component type migrated on load: `count` blocks went from version `from` to `to`.
struct UpgradeApplied {
    std::string component;
    uint32_t from{0};
    uint32_t to{0};
    std::size_t count{0};
};

struct LoadReport {
    std::size_t entities{0};
    std::size_t preservedUnknown{0};          // unknown component blocks kept as-is
    std::vector<UpgradeApplied> upgraded;     // in registration order
    std::vector<Diagnostic> warnings;
};

struct ValidationReport {
    std::vector<Diagnostic> errors;
    std::vector<Diagnostic> warnings;
    std::vector<UpgradeApplied> upgraded;     // what a load would migrate
    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

// Fails (E_NONFINITE / E_ENUM, with the entity's path) only on a value that cannot be
// written canonically.
[[nodiscard]] Result<std::string, Diagnostic> saveScene(const World& world, const reflect::TypeRegistry& types,
                                                        SaveOptions options = {}, SaveReport* report = nullptr);

// Phase 1 only: every error and warning a load would report, without a World.
[[nodiscard]] ValidationReport validateScene(std::string_view text, const reflect::TypeRegistry& types);

// `dst` must be empty (E_WORLD_NOT_EMPTY otherwise). On error `dst` is unchanged.
[[nodiscard]] Result<LoadReport, std::vector<Diagnostic>> loadScene(World& dst, std::string_view text,
                                                                    const reflect::TypeRegistry& types);

// UTF-8, LF, trailing newline (Pretty). E_IO when the file cannot be written or read.
[[nodiscard]] Result<bool, Diagnostic> saveSceneFile(const World& world, const reflect::TypeRegistry& types,
                                                     const std::filesystem::path& path, SaveOptions options = {});
[[nodiscard]] Result<LoadReport, std::vector<Diagnostic>> loadSceneFile(World& dst, const std::filesystem::path& path,
                                                                        const reflect::TypeRegistry& types);

// A compact JSON array of diagnostics (the shape CLI tools and agents read).
[[nodiscard]] std::string diagnosticsJson(std::span<const Diagnostic> diagnostics);

} // namespace ohao::ecs
