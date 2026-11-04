// The pipeline disk cache's files (render/rt/pipeline_disk_cache.hpp): one file per shader version, so a cache
// never accumulates the entries of shaders since changed (it once passed its 64 MB cap and every process then
// compiled the path tracer cold, minutes each), and the old versions of the same pipeline pruned.
#include <gtest/gtest.h>
#include "render/rt/pipeline_disk_cache.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ohao;
namespace fs = std::filesystem;

namespace {

fs::path scratch(const char* name) {
    fs::path d = fs::temp_directory_path() / ("ohao_pipeline_cache_test_" + std::string(name));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

void touch(const fs::path& p, int ageSeconds) {
    std::ofstream(p, std::ios::binary) << "x";
    fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::seconds(ageSeconds));
}

}  // namespace

// A different byte anywhere in any module is another version; the same modules the same key.
TEST(PipelineCacheFiles, KeyFollowsTheShaders) {
    const std::vector<std::vector<char>> a{{'a', 'b'}, {'c'}};
    std::vector<std::vector<char>> b = a;
    EXPECT_EQ(pipelineContentKey(a), pipelineContentKey(b));
    b[1][0] = 'd';
    EXPECT_NE(pipelineContentKey(a), pipelineContentKey(b));
    // Moving a byte from one module to the next is another version too.
    const std::vector<std::vector<char>> c{{'a'}, {'b', 'c'}};
    EXPECT_NE(pipelineContentKey(a), pipelineContentKey(c));
}

// One pipeline's name is never a prefix match for another's (path_tracer, path_tracer_caustics).
TEST(PipelineCacheFiles, NamesDoNotCollide) {
    const std::string a = pipelineCacheFileName("path_tracer", 0x1234, 4318, 11608, 77);
    const std::string b = pipelineCacheFileName("path_tracer_caustics", 0x1234, 4318, 11608, 77);
    EXPECT_NE(a, b);
    EXPECT_NE(b.rfind(a.substr(0, a.find('@') + 1), 0), 0u);
    EXPECT_EQ(a, "path_tracer@0000000000001234-4318-11608-77.bin");
}

// Saving one version prunes that pipeline's other versions on this device and driver, but the newest of them
// (switching back and forth stays warm), and the files named before versions were (no '@'); other pipelines'
// files and other devices' stay.
TEST(PipelineCacheFiles, PruneKeepsTheCurrentAndNewestOther) {
    const fs::path d = scratch("prune");
    const fs::path current = d / pipelineCacheFileName("path_tracer", 5, 1, 2, 3);
    touch(current, 0);
    touch(d / pipelineCacheFileName("path_tracer", 4, 1, 2, 3), 10);
    touch(d / pipelineCacheFileName("path_tracer", 3, 1, 2, 3), 20);
    touch(d / pipelineCacheFileName("path_tracer", 2, 1, 2, 3), 30);
    touch(d / "path_tracer-1-2-3.bin", 40);   // the old naming
    const fs::path caustics = d / pipelineCacheFileName("path_tracer_caustics", 9, 1, 2, 3);
    touch(caustics, 50);
    const fs::path otherDevice = d / pipelineCacheFileName("path_tracer", 1, 1, 2, 99);
    touch(otherDevice, 60);

    prunePipelineCaches(d, "path_tracer", "-1-2-3.bin", current, 1);

    EXPECT_TRUE(fs::exists(current));
    EXPECT_TRUE(fs::exists(d / pipelineCacheFileName("path_tracer", 4, 1, 2, 3)));
    EXPECT_FALSE(fs::exists(d / pipelineCacheFileName("path_tracer", 3, 1, 2, 3)));
    EXPECT_FALSE(fs::exists(d / pipelineCacheFileName("path_tracer", 2, 1, 2, 3)));
    EXPECT_FALSE(fs::exists(d / "path_tracer-1-2-3.bin"));
    EXPECT_TRUE(fs::exists(caustics));
    EXPECT_TRUE(fs::exists(otherDevice));
    std::error_code ec;
    fs::remove_all(d, ec);
}
