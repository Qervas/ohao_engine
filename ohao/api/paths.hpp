#pragma once
// The file sandbox of a session that serves a remote client (sub-project 4 spec,
// "Security"). Paths in params are project-relative with '/' (no absolute paths, no
// ".."), resolved against the root; reads stay inside the root, writes inside the output
// directory. An --allow-path root admits reads and writes, and absolute paths, inside it.
// Anything else is E_ACCESS. A session without a policy (the CLI) uses paths as given.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::api {

struct PathPolicy {
    std::filesystem::path root;                   // reads; relative paths resolve here
    std::filesystem::path out;                    // writes (default <root>/agent_out)
    std::vector<std::filesystem::path> allowed;   // extra roots, reads and writes
};

enum class PathUse : unsigned char { Read, Write };

// Makes every root absolute and canonical (the output directory is created).
[[nodiscard]] PathPolicy normalizePolicy(PathPolicy p);

// The file `path` names under `policy`, or an empty path with `why` set. A write's
// parent directories are created when the path is admitted.
[[nodiscard]] std::filesystem::path resolvePath(const PathPolicy& policy, std::string_view path, PathUse use,
                                                std::string& why);

} // namespace ohao::api
