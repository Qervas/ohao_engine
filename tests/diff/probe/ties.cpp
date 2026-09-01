// The eight startup source-parsing ties.
//
// Every one of these reads a SHADER SOURCE off disk and binds a constant, a
// layout or a spelling in it to the host's picture of the same thing. They
// run before any Vulkan object exists, in the order main() calls them, and
// they resolve their shader paths by climbing parent directories from the
// PROCESS WORKING DIRECTORY -- not from this file's location -- so moving
// them into their own translation unit does not change what they find.
//
// Lifted verbatim out of diff_gpu_probe.cpp. The four constants they tie
// against on the C++ side (kShaderGrazingCos, kBsdfProbeFloatsPerCase,
// kParityRayTMax, kParitySurfaceOffset) now reach them through
// probe/oracle_bsdf.hpp and probe/oracle_integrator.hpp instead of file
// scope. That is a linkage change; the values are the same.
#include "probe/ties.hpp"

#include "probe/oracle_bsdf.hpp"
#include "probe/oracle_integrator.hpp"

#include "gpu_probe_context.hpp"

#include "diff/param/param_registry.hpp"
#include "diff/rng/diff_rng.hpp"
#include "diff/wavefront/wavefront_loop.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace ohao::diff::probe {

/// Bind ohao::diff::kNeeSampleFloats to wf_scatter.comp's OWN
/// kNeeSampleFloats, at runtime, by reading the declaration out of the
/// shader source.
///
/// WHY A RUNTIME CHECK AND NOT A static_assert. The two constants live on
/// opposite sides of the GLSL/C++ boundary. GLSL has no static_assert; the
/// value is folded into unnamed SPIR-V literals, so it cannot be reflected
/// out of the compiled module under a name either; and there is no
/// generated header in this build that both sides could include. What is
/// left is the source text, which is authoritative because it is what the
/// shader compiler consumed. A mismatch is a SILENT wrong-slot read -- the
/// host would stride the readback by one number while the GPU wrote it with
/// another, producing plausible-looking garbage rather than a validation
/// error -- so this fails the whole probe rather than warning.
///
/// The search climbs parent directories looking for the SOURCE tree. Note
/// this is NOT the same mechanism as ComputePipeline::loadSpv, which
/// enumerates fixed sibling roots (bin/shaders/, build/Release/bin/shaders/)
/// for compiled SPV BINARIES -- an earlier comment here claimed they matched
/// and they do not. Not finding the source is itself a failure, because "the
/// tie could not be checked" must not be allowed to read as "the tie holds".
///
/// The declaration is looked for in a COMMENT-STRIPPED copy of the source,
/// not the raw text. Scanning raw text would match a commented-out
/// declaration -- `// const uint kNeeSampleFloats = 21u;` -- and report a
/// live, tied constant even if the real one had been renamed or deleted.
/// Commenting a declaration out is precisely how such a constant goes
/// missing, so the one case this check exists to catch is the one raw
/// scanning would miss.
///
/// Shared by checkNeeStrideTie, checkScatterPushSizeTie and
/// checkParityRefConstantsTie below: all are GLSL/C++ ties against a shader
/// source, and all need the same comment-stripping for the same reason (see
/// the previous paragraph), so this is the one place that logic lives rather
/// than several copies that could drift apart from each other. The FILE is a
/// parameter because the constants those checks tie do not all live in one
/// shader -- wf_intersect.comp owns the path ray's tMax, wf_scatter.comp the
/// shadow ray's -- and a loader hard-wired to one file is what let the
/// second of those go untied.
bool loadShaderSourceStripped(const char* relativePath, std::string& outStripped,
                              std::string& outFoundPath) {
    static const char* const kPrefixes[] = {"", "../", "../../", "../../../"};
    std::string text;
    std::string found;
    bool haveFound = false;
    for (const char* prefix : kPrefixes) {
        const std::string candidate = std::string(prefix) + relativePath;
        std::ifstream in(candidate, std::ios::binary);
        if (!in.is_open()) continue;
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        found = candidate;
        haveFound = true;
        break;
    }
    if (!haveFound) return false;
    outFoundPath = found;

    // Strip GLSL comments before matching. GLSL has no string literals, so a
    // two-state scan over // and /* */ is exact here. Newlines are preserved
    // so that a regex's [ \t] (which cannot span lines) still rejects a
    // reformatted multi-line declaration.
    std::string stripped;
    stripped.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
        } else if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
                if (text[i] == '\n') stripped.push_back('\n');
                ++i;
            }
            i = (i + 1 < text.size()) ? i + 2 : text.size();
        } else {
            stripped.push_back(text[i]);
            ++i;
        }
    }
    outStripped = std::move(stripped);
    return true;
}

/// The scatter TRAVERSAL case, named because four checks want it.
///
/// This used to load shaders/diff/wf_scatter.comp, and every constant, sink
/// write and Push field the four ties below parse used to live there. Stage 1
/// Task 1 extracted the whole traversal into shaders/includes/diff/traverse.glsl
/// so that the forward and replay instantiations cannot diverge (spec section
/// 6.2), and the declarations moved with the code. Re-pointing this loader is
/// what keeps those four ties tied to the text the shader compiler actually
/// consumes; leaving it aimed at wf_scatter.comp would have made every one of
/// them fail closed (the declarations are simply not there any more), which is
/// the right failure mode but not the right file.
///
/// The ties are STRONGER for the move, not weaker: they now cover the one
/// source BOTH instantiations compile, so a drift they would catch cannot hide
/// in whichever of the two files a reader did not open.
bool loadDiffTraverseSourceStripped(std::string& outStripped, std::string& outFoundPath) {
    return loadShaderSourceStripped("shaders/includes/diff/traverse.glsl", outStripped,
                                    outFoundPath);
}

bool checkNeeStrideTie() {
    std::string stripped, found;
    if (!loadDiffTraverseSourceStripped(stripped, found)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: could not open shaders/includes/diff/traverse.glsl from any "
                     "candidate path, so the binding-7 record stride could not be tied to "
                     "ohao::diff::kNeeSampleFloats (%u). An unchecked tie is not a held tie: a "
                     "mismatch is a silent wrong-slot read, not a validation error\n",
                     ohao::diff::kNeeSampleFloats);
        return false;
    }

    const std::regex decl(R"(const[ \t]+uint[ \t]+kNeeSampleFloats[ \t]*=[ \t]*([0-9]+)u[ \t]*;)");
    std::smatch m;
    if (!std::regex_search(stripped, m, decl)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s no longer declares `const uint kNeeSampleFloats = "
                     "<N>u;` on one line, so the binding-7 record stride cannot be tied to "
                     "ohao::diff::kNeeSampleFloats (%u). Restore the spelling or update this "
                     "check -- do not leave the two constants untied\n",
                     found.c_str(), ohao::diff::kNeeSampleFloats);
        return false;
    }
    const unsigned long shaderStride = std::stoul(m[1].str());
    if (shaderStride != static_cast<unsigned long>(ohao::diff::kNeeSampleFloats)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s writes %lu floats per path into binding 7 while "
                     "ohao::diff::kNeeSampleFloats says %u. The host sizes the buffer and strides "
                     "the readback by its own number, so every slot past path 0 would be read at "
                     "the wrong offset and every check below would be measuring the wrong "
                     "floats\n",
                     found.c_str(), shaderStride, ohao::diff::kNeeSampleFloats);
        return false;
    }
    std::printf("[diff_gpu_probe] NOTE: binding-7 record stride tied -- %s declares %lu floats per "
                "path and ohao::diff::kNeeSampleFloats is %u\n",
                found.c_str(), shaderStride, ohao::diff::kNeeSampleFloats);
    return true;
}

/// Collapses every run of whitespace in `text` to one space and trims the
/// ends. Used to compare a GLSL right-hand side against an expected spelling
/// without making the tie sensitive to reformatting.
std::string squashWhitespace(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) out.push_back(' ');
        pendingSpace = false;
        out.push_back(c);
    }
    return out;
}

/// One expected `neeSamples.v[nb + <offset>u] = <rhs>;` write.
struct NeeSlotExpectation {
    std::uint32_t offset;
    const char* rhs;
};

/// Ties wf_scatter.comp's THREE probe-only sinks -- the binding-7 next-event
/// record, the binding-3 debug-draw record and the binding-6 environment
/// record -- to the host's picture of them, SLOT BY SLOT rather than only by
/// stride.
///
/// WHY THIS EXISTS (review finding, whole-branch review of Stage 0b-2b).
/// checkNeeStrideTie already ties the STRIDE, kNeeSampleFloats = 25. Nothing
/// tied the 24 named OFFSETS, and those offsets are what checks 28, 29, 30,
/// 31 and 32 index the readback by. wf_scatter.comp's own write block carried
/// the comment "Every slot is named here and in gpu_probe_context.hpp's
/// NeeSampleSlot enum; they must not drift" -- verbatim the situation
/// checkNeeStrideTie's header argues a comment cannot hold. Concretely: a
/// TRANSPOSITION of two same-arity slots keeps the stride at 25 and passes
/// every existing check silently. Swap the x and y channels of
/// kNeeSlotArrivalThroughput and check 32 compares all three against one
/// shared expected value, so it cannot see it; swap kNeeSlotVisLight with
/// kNeeSlotVisBsdf and check 28 requires both to be 0 anyway.
///
/// HOW IT TIES. Every slot's RIGHT-HAND SIDE is named here, next to the C++
/// enumerator whose value indexes it. A transposition changes which
/// expression lands at which offset, so it changes the RHS at both offsets
/// and is rejected. The expectation table is keyed by the enumerators
/// themselves (kNeeSlotBsdfDir + 1u, not the literal 17), so renumbering the
/// C++ enum without moving the shader's write -- the other half of the same
/// drift -- is rejected too.
///
/// WHAT IT DOES NOT TIE, stated so no one has to infer it:
///   * The MEANING of an expression. This check knows `visBsdf` must land at
///     kNeeSlotVisBsdf; it cannot know that wf_scatter.comp computed
///     `visBsdf` from the BSDF sample's shadow ray rather than the light
///     sample's. That is checks 28-31's job and they do it on a GPU run.
///   * Anything about the FILM (binding 9). Its stride is 3 floats per pixel
///     on both sides and is untied; check 32 measures the film's contents
///     against a host reconstruction, which a wrong stride would break
///     loudly rather than silently.
///   * kDrawsPerBounce (5). Untied on purpose -- it is covered by
///     MEASUREMENT instead: checks 15 and 18 compare the host's replayed
///     PathRng draw count against the count the shader itself reports
///     through the binding-3 DEBUG record's slot 2 (wf_scatter.comp writes
///     debugDraws.v[pathIndex*3u + 2u]) -- NOT through any binding-7 sink
///     slot, so
///     a change to it fails those checks rather than passing quietly.
bool checkWfScatterSinkLayoutTie() {
    std::string stripped, found;
    if (!loadDiffTraverseSourceStripped(stripped, found)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: could not open shaders/includes/diff/traverse.glsl from any "
                     "candidate path, so its per-slot sink layout could not be tied to "
                     "gpu_probe_context.hpp's NeeSampleSlot enum. An unchecked tie is not a held "
                     "tie\n");
        return false;
    }

    // --- The binding-7 next-event record, slot by slot. ---
    const std::regex neeWrite(
        R"(neeSamples\.v\[[ \t]*nb[ \t]*\+[ \t]*([0-9]+)u[ \t]*\][ \t]*=([^;]*);)");
    std::map<std::uint32_t, std::string> writes;
    for (auto it = std::sregex_iterator(stripped.begin(), stripped.end(), neeWrite);
         it != std::sregex_iterator(); ++it) {
        const std::uint32_t offset = static_cast<std::uint32_t>(std::stoul((*it)[1].str()));
        const std::string rhs = squashWhitespace((*it)[2].str());
        if (!writes.emplace(offset, rhs).second) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %s writes binding-7 slot %u more than once. The "
                         "record is a single write block by construction; two writes to one slot "
                         "mean the host's picture of which value lives where cannot be derived "
                         "from the source at all\n",
                         found.c_str(), offset);
            return false;
        }
    }
    if (writes.size() != ohao::diff::kNeeSampleFloats) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s has %zu `neeSamples.v[nb + <N>u] = ...;` writes "
                     "but ohao::diff::kNeeSampleFloats is %u. Every slot must be written by "
                     "exactly one statement this check can see -- an unwritten slot is read back "
                     "as whatever the allocator handed over, and a slot written through a "
                     "spelling this regex does not match is untied\n",
                     found.c_str(), writes.size(), ohao::diff::kNeeSampleFloats);
        return false;
    }

    // The enumerators, not the literals: this is the half of the tie that
    // catches a C++-side renumbering.
    const NeeSlotExpectation kExpected[] = {
        {ohao::diff::kNeeSlotNeeUnweighted + 0u, "neeTerm.unweighted.x"},
        {ohao::diff::kNeeSlotNeeUnweighted + 1u, "neeTerm.unweighted.y"},
        {ohao::diff::kNeeSlotNeeUnweighted + 2u, "neeTerm.unweighted.z"},
        {ohao::diff::kNeeSlotWEnvAtLight, "neeTerm.wOwn"},
        {ohao::diff::kNeeSlotWBsdfAtLight, "neeTerm.wOther"},
        {ohao::diff::kNeeSlotBsdfUnweighted + 0u, "bsdfTerm.unweighted.x"},
        {ohao::diff::kNeeSlotBsdfUnweighted + 1u, "bsdfTerm.unweighted.y"},
        {ohao::diff::kNeeSlotBsdfUnweighted + 2u, "bsdfTerm.unweighted.z"},
        {ohao::diff::kNeeSlotWBsdfAtBsdf, "bsdfTerm.wOwn"},
        {ohao::diff::kNeeSlotWEnvAtBsdf, "bsdfTerm.wOther"},
        {ohao::diff::kNeeSlotEnvRadiance, "envRadiance"},
        {ohao::diff::kNeeSlotPdfEnvAtBsdf, "pdfEnvAtBsdfDir"},
        {ohao::diff::kNeeSlotPdfBsdfAtLight, "pdfBsdfAtEnvDir"},
        {ohao::diff::kNeeSlotVisLight, "visEnv"},
        {ohao::diff::kNeeSlotVisBsdf, "visBsdf"},
        {ohao::diff::kNeeSlotSurfaceBranch, "surfaceBranch"},
        {ohao::diff::kNeeSlotBsdfDir + 0u, "bsdfSampleDir.x"},
        {ohao::diff::kNeeSlotBsdfDir + 1u, "bsdfSampleDir.y"},
        {ohao::diff::kNeeSlotBsdfDir + 2u, "bsdfSampleDir.z"},
        {ohao::diff::kNeeSlotPdfBsdfAtBsdf, "bsdfSamplePdf"},
        {ohao::diff::kNeeSlotBsdfRadiance, "bsdfRadiance"},
        {ohao::diff::kNeeSlotArrivalThroughput + 0u, "arrivalThroughput.x"},
        {ohao::diff::kNeeSlotArrivalThroughput + 1u, "arrivalThroughput.y"},
        {ohao::diff::kNeeSlotArrivalThroughput + 2u, "arrivalThroughput.z"},
        {ohao::diff::kNeeSlotPixelIndex, "float(pixelIndex)"},
    };
    static_assert(sizeof(kExpected) / sizeof(kExpected[0]) == ohao::diff::kNeeSampleFloats,
                  "the expectation table must name every slot of the record");

    for (const NeeSlotExpectation& e : kExpected) {
        const auto it = writes.find(e.offset);
        if (it == writes.end()) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %s never writes binding-7 slot %u, which "
                         "gpu_probe_context.hpp's NeeSampleSlot enum says holds `%s`. Every check "
                         "that reads that slot would be reading uninitialised memory\n",
                         found.c_str(), e.offset, e.rhs);
            return false;
        }
        if (it->second != e.rhs) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %s writes `%s` into binding-7 slot %u, but "
                         "gpu_probe_context.hpp's NeeSampleSlot enum places `%s` there. The "
                         "stride still agrees, so nothing else in this probe would have noticed: "
                         "checks 28-32 would keep reading well-formed floats out of the wrong "
                         "slots\n",
                         found.c_str(), it->second.c_str(), e.offset, e.rhs);
            return false;
        }
    }

    // --- The binding-3 debug-draw record and the binding-6 env record. ---
    // Same shape, one stride and a fixed set of offsets each.
    struct SinkTie {
        const char* buffer;
        const char* purpose;
        std::uint32_t hostStride;
        const char* hostConstant;
    };
    const SinkTie kSinks[] = {
        {"debugDraws", "binding 3, the RNG debug record", ohao::diff::kDebugDrawFloats,
         "ohao::diff::kDebugDrawFloats"},
        {"envSamples", "binding 6, the environment-sample record", ohao::diff::kEnvSampleFloats,
         "ohao::diff::kEnvSampleFloats"},
    };
    std::uint32_t debugStride = 0;
    std::uint32_t envStride = 0;
    for (const SinkTie& sink : kSinks) {
        const std::regex write(std::string(sink.buffer) +
                               R"(\.v\[[ \t]*pathIndex[ \t]*\*[ \t]*([0-9]+)u[ \t]*\+[ \t]*([0-9]+)u[ \t]*\][ \t]*=)");
        std::uint32_t shaderStride = 0;
        bool haveStride = false;
        std::set<std::uint32_t> offsets;
        for (auto it = std::sregex_iterator(stripped.begin(), stripped.end(), write);
             it != std::sregex_iterator(); ++it) {
            const std::uint32_t stride = static_cast<std::uint32_t>(std::stoul((*it)[1].str()));
            const std::uint32_t offset = static_cast<std::uint32_t>(std::stoul((*it)[2].str()));
            if (haveStride && stride != shaderStride) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: %s strides `%s` (%s) by both %u and %u. One "
                             "record cannot have two strides; the host reads it with one\n",
                             found.c_str(), sink.buffer, sink.purpose, shaderStride, stride);
                return false;
            }
            shaderStride = stride;
            haveStride = true;
            if (!offsets.insert(offset).second) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: %s writes `%s` (%s) offset %u more than "
                             "once\n",
                             found.c_str(), sink.buffer, sink.purpose, offset);
                return false;
            }
        }
        if (!haveStride) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %s no longer writes `%s.v[pathIndex * <N>u + "
                         "<K>u] = ...` in the spelling this check looks for (%s), so its stride "
                         "cannot be tied to %s. Restore the spelling or update this check -- do "
                         "not leave it untied\n",
                         found.c_str(), sink.buffer, sink.purpose, sink.hostConstant);
            return false;
        }
        if (shaderStride != sink.hostStride) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %s strides `%s` (%s) by %u floats per path while "
                         "%s says %u. The host sizes the buffer and strides the readback by its "
                         "own number, so every record past path 0 would be read at the wrong "
                         "offset\n",
                         found.c_str(), sink.buffer, sink.purpose, shaderStride, sink.hostConstant,
                         sink.hostStride);
            return false;
        }
        if (offsets.size() != sink.hostStride || *offsets.begin() != 0u ||
            *offsets.rbegin() != sink.hostStride - 1u) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %s writes %zu distinct offsets into `%s` (%s) "
                         "with a stride of %u. A record with an unwritten slot is read back as "
                         "whatever the allocator handed over\n",
                         found.c_str(), offsets.size(), sink.buffer, sink.purpose, sink.hostStride);
            return false;
        }
        if (std::strcmp(sink.buffer, "debugDraws") == 0) debugStride = shaderStride;
        else envStride = shaderStride;
    }

    // --- The binding-3 VERTEX TRACE record, slot by slot (Stage 1 Task 1).
    //
    // The stride check above cannot see a TRANSPOSITION: swapping the origin
    // and dir triples, or writing `pathThroughput` where `hitT` belongs,
    // leaves the stride at kDebugDrawFloats and every offset written exactly
    // once. That is precisely the defect this record cannot survive, because
    // the replay-equivalence check compares two runs SLOT BY SLOT: two
    // instantiations sharing one traversal transpose IDENTICALLY and so still
    // agree, meaning the comparison would pass while the host's picture of
    // which value lives where was wrong -- and the throughput assertion that
    // establishes the comparison is not ranging over zeros would then be
    // reading a direction component. Binding 7 got exactly this treatment for
    // exactly this reason; the trace record now carries the same weight and
    // gets the same tie.
    struct TraceSlotExpectation {
        std::uint32_t offset;
        const char* rhs;
    };
    const TraceSlotExpectation kTraceExpected[] = {
        {ohao::diff::kTraceSlotU1, "u1"},
        {ohao::diff::kTraceSlotU2, "u2"},
        {ohao::diff::kTraceSlotDrawCount, "uintBitsToFloat(rng.draws)"},
        {ohao::diff::kTraceSlotULobe, "uLobe"},
        {ohao::diff::kTraceSlotUEnv1, "uEnv1"},
        {ohao::diff::kTraceSlotUEnv2, "uEnv2"},
        {ohao::diff::kTraceSlotOrigin + 0u, "origin.x"},
        {ohao::diff::kTraceSlotOrigin + 1u, "origin.y"},
        {ohao::diff::kTraceSlotOrigin + 2u, "origin.z"},
        {ohao::diff::kTraceSlotDir + 0u, "dir.x"},
        {ohao::diff::kTraceSlotDir + 1u, "dir.y"},
        {ohao::diff::kTraceSlotDir + 2u, "dir.z"},
        {ohao::diff::kTraceSlotThroughput + 0u, "pathThroughput.x"},
        {ohao::diff::kTraceSlotThroughput + 1u, "pathThroughput.y"},
        {ohao::diff::kTraceSlotThroughput + 2u, "pathThroughput.z"},
        {ohao::diff::kTraceSlotHitT, "hitT"},
        {ohao::diff::kTraceSlotBounce, "uintBitsToFloat(bounce)"},
        {ohao::diff::kTraceSlotPixelIndex, "uintBitsToFloat(pixelIndex)"},
    };
    static_assert(sizeof(kTraceExpected) / sizeof(kTraceExpected[0]) ==
                      ohao::diff::kDebugDrawFloats,
                  "the expectation table must name every slot of the trace record");

    const std::regex traceWrite(
        R"(debugDraws\.v\[[ \t]*pathIndex[ \t]*\*[ \t]*[0-9]+u[ \t]*\+[ \t]*([0-9]+)u[ \t]*\][ \t]*=([^;]*);)");
    std::map<std::uint32_t, std::string> traceWrites;
    for (auto it = std::sregex_iterator(stripped.begin(), stripped.end(), traceWrite);
         it != std::sregex_iterator(); ++it) {
        const std::uint32_t offset = static_cast<std::uint32_t>(std::stoul((*it)[1].str()));
        traceWrites[offset] = squashWhitespace((*it)[2].str());
    }
    for (const TraceSlotExpectation& e : kTraceExpected) {
        const auto it = traceWrites.find(e.offset);
        if (it == traceWrites.end()) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %s never writes binding-3 slot %u, which "
                         "gpu_probe_context.hpp's TraceSlot enum says holds `%s`\n",
                         found.c_str(), e.offset, e.rhs);
            return false;
        }
        if (it->second != e.rhs) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %s writes `%s` into binding-3 slot %u, but "
                         "gpu_probe_context.hpp's TraceSlot enum places `%s` there. The stride "
                         "still agrees, so nothing else would have noticed -- and the "
                         "replay-equivalence check would keep comparing two runs that transposed "
                         "the SAME two slots and so keep agreeing\n",
                         found.c_str(), it->second.c_str(), e.offset, e.rhs);
            return false;
        }
    }

    std::printf("[diff_gpu_probe] NOTE: the scatter traversal's probe sinks tied slot by slot -- "
                "%s writes all %u binding-7 slots at the offsets NeeSampleSlot names AND all %u "
                "binding-3 slots at the offsets TraceSlot names, each carrying the expression "
                "that enumerator documents (so a transposition of two same-arity slots, which "
                "leaves the strides at %u and %u, is rejected); binding 6 strides %u, matching "
                "kEnvSampleFloats\n",
                found.c_str(), ohao::diff::kNeeSampleFloats, ohao::diff::kDebugDrawFloats,
                ohao::diff::kNeeSampleFloats, debugStride, envStride);
    return true;
}

/// Ties `ohao::diff::kDrawsPerBounce` to the traversal's own declaration, the
/// same way `checkNeeStrideTie` ties the binding-7 stride and for a sharper
/// reason.
///
/// This number is what the CPU-side `ohao::diff::PathRng` oracle FAST-FORWARDS
/// BY before comparing a bounce's draws. If the shader's value and the host's
/// disagree, the oracle walks a different stream than the shader from bounce 1
/// onward -- so the checks that use it are not comparing a GPU stream against
/// a CPU stream at all, they are comparing two unrelated streams, and their
/// agreement or disagreement means nothing either way. That makes it exactly
/// the "expected value derived from the same source as the measured value"
/// shape, one level removed: the number that positions BOTH sides.
///
/// It is also the number the replay rests on. `kDrawsPerBounce` must be
/// branch-independent -- the lobe draw unconditional, both environment draws
/// taken before the miss guard -- so that a hit and a miss consume identical
/// stream positions and the fast-forward is a pure function of `bounce`. That
/// property is not checkable from a literal, but the literal being wrong makes
/// every check of it vacuous, so this is where the chain starts.
bool checkDrawsPerBounceTie() {
    std::string stripped, found;
    if (!loadDiffTraverseSourceStripped(stripped, found)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: could not open shaders/includes/diff/traverse.glsl "
                     "from any candidate path, so the per-bounce RNG draw count could not be tied "
                     "to ohao::diff::kDrawsPerBounce (%u). An unchecked tie is not a held tie\n",
                     ohao::diff::kDrawsPerBounce);
        return false;
    }
    const std::regex decl(R"(const[ \t]+uint[ \t]+kDrawsPerBounce[ \t]*=[ \t]*([0-9]+)u[ \t]*;)");
    std::smatch m;
    if (!std::regex_search(stripped, m, decl)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s no longer declares `const uint kDrawsPerBounce = "
                     "<N>u;` on one line, so the host's PathRng fast-forward cannot be tied to "
                     "the shader's. Restore the spelling or update this check -- do not leave the "
                     "two untied\n",
                     found.c_str());
        return false;
    }
    const unsigned long shaderDraws = std::stoul(m[1].str());
    if (shaderDraws != static_cast<unsigned long>(ohao::diff::kDrawsPerBounce)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s draws %lu values per bounce while "
                     "ohao::diff::kDrawsPerBounce says %u. Every CPU-side PathRng oracle in this "
                     "probe fast-forwards by the host's number, so from bounce 1 onward it would "
                     "be comparing a position in the stream the shader never occupied\n",
                     found.c_str(), shaderDraws, ohao::diff::kDrawsPerBounce);
        return false;
    }
    std::printf("[diff_gpu_probe] NOTE: per-bounce RNG draw count tied -- %s draws %lu values per "
                "bounce and ohao::diff::kDrawsPerBounce is %u\n",
                found.c_str(), shaderDraws, ohao::diff::kDrawsPerBounce);
    return true;
}

/// Ties the "ONE SOURCE, TWO INSTANTIATIONS" claim (spec section 6.2) to the
/// two files that make it, by REFUSING TO RUN unless neither of them can
/// diverge from the other.
///
/// WHY THIS IS A CHECK AND NOT A COMMENT. The whole of Stage 1 rests on the
/// forward and replay kernels walking the identical path, consuming the
/// identical RNG values in the identical order. Spec section 6.2's answer is
/// structural -- "the traversal is one piece of source, included twice, with a
/// per-vertex hook the includer defines. Divergence is made structurally
/// impossible rather than prevented by discipline." But "structurally
/// impossible" is a property of the FILES, and files drift: a binding added to
/// one instantiation and not the other, a Push field, a `#define` that switches
/// a branch inside the shared source, a second traversal quietly written next
/// to the hook. Each of those reintroduces exactly the divergence the design
/// eliminated, and NONE of them would fail to compile.
///
/// The replay-equivalence check downstream cannot catch them either, and that
/// is the subtle part: it compares two runs of the shared traversal, so a
/// change that reaches BOTH instantiations changes both records identically
/// and they still agree. Only a check on the SOURCE can see that the two
/// instantiations stopped being two instantiations.
///
/// WHICH FILES ARE CHECKED -- DISCOVERED, NOT LISTED (review Finding 2). This
/// used to name the two `.comp` paths in a hardcoded array, which left a
/// THIRD file including the traversal completely unchecked: it could declare
/// its own `layout(...)`, define a second top-level function, or carry the
/// `#define` half of a `#define`/`#ifdef` pair straight into the shared
/// source -- the precise sabotage rule 5 exists to catch, reintroduced
/// through a file this check did not know existed. Worse, rule 5 ACTIVELY
/// PUSHES a future compile-time variant toward writing exactly such a file,
/// which turns a good rule into a trap. So the set is now globbed:
/// `shaders/diff/*.comp` is enumerated, every file whose comment-stripped
/// text includes `diff/traverse.glsl` is an instantiation, and ALL of them
/// must satisfy every rule below. Fewer than two discovered is itself a
/// failure -- deleting an instantiation must not be a way to pass.
///
/// WHAT IS ENFORCED, on the comment-stripped text of every discovered file:
///
///   1. exactly ONE `#include`, and it is `diff/traverse.glsl`;
///   2. no `layout(...)` declaration -- no bindings, no push constants, no
///      local size. All of those live in the shared source, so neither
///      instantiation can grow a descriptor interface the other lacks or a
///      Push block that disagrees with `WavefrontLoop::ScatterPush`;
///   3. the ONLY top-level function definitions are `diffVertexHook` and
///      `main` -- a second traversal cannot hide beside the hook;
///   4. `main` is exactly `void main() { diffTraverse(); }`, so neither
///      instantiation can do work before or after the shared traversal;
///   5. no preprocessor directive other than `#version` and that one
///      `#include`. This is the one that keeps the shared source from being
///      CONFIGURABLE per instantiation: a `#define` in one `.comp` plus an
///      `#ifdef` in traverse.glsl is textually one source and behaviourally
///      two, and it is the cheapest way for a future task to reintroduce a
///      per-instantiation RNG draw.
///   6. every discovered instantiation declares the SAME `#version`. Rule 5
///      permits `#version` because a `.comp` must have one; permitting it
///      without comparing them left two files free to compile the shared
///      source under two language versions, which is a behavioural
///      difference in one source (review Finding 4b). Both are 460 today.
///
/// Rule 3 is deliberately about DEFINITIONS at column 0. A hook body may call
/// whatever it likes; what it may not do is define a second entry point.
///
/// WHERE THIS IS STILL NOT AIRTIGHT -- read it, do not assume the five rules
/// close every channel:
///
///   * A HOOK BODY IS UNCONSTRAINED. Everything traverse.glsl declares is in
///     scope inside a hook, so one can call psSetOrigin/psSetDir/
///     psSetThroughput/psSetBounce or write `queues`/`counters` and diverge
///     the NEXT bounce, leaving this bounce's record identical. Rule 3 does
///     not see it (it constrains DEFINITIONS, not calls) and the
///     replay-equivalence check sees it only at bounce b+1, and only if a
///     bounce b+1 is run. traverse.glsl's DiffVertex comment states the
///     may/may-not as an explicit contract for that reason -- Task 2's hook
///     legitimately writes a gradient arena, so the boundary has to be
///     written down before it is crossed.
///   * NOTHING CHECKS traverse.glsl ITSELF for conditionals beyond its
///     include guard. Rule 5 keeps the `#define` half out of the `.comp`
///     files; an `#ifdef` in the shared source with no `#define` anywhere is
///     inert, but the pair is only half-prevented.
///   * NOTHING CHECKS THE COMPILE COMMAND LINES. A per-file `-D` would
///     reopen the same sabotage from the other end, with no `.comp` edit at
///     all. What makes that safe TODAY is shaders/CMakeLists.txt emitting one
///     identical `glslc` line for every shader with no per-file options --
///     a property of the build script, not of this check.
bool checkTraverseInstantiationTie() {
    // --- DISCOVERY: find the shader directory, then every `.comp` in it. The
    // prefixes are loadShaderSourceStripped's, for the same reason -- the
    // probe runs from build/Release and the source tree is some number of
    // parents up. Not finding the directory is a FAILURE, not a skip: a check
    // that globs and finds nothing knows nothing.
    static const char* const kInstantiationPrefixes[] = {"", "../", "../../", "../../../"};
    std::string dir;
    for (const char* prefix : kInstantiationPrefixes) {
        std::error_code ec;
        const std::string candidate = std::string(prefix) + "shaders/diff";
        if (std::filesystem::is_directory(candidate, ec)) {
            dir = candidate;
            break;
        }
    }
    if (dir.empty()) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: could not locate the shaders/diff directory from any "
                     "candidate path, so the set of files including the shared traversal could "
                     "not be ENUMERATED. This check globs rather than naming files precisely so "
                     "that a third instantiation cannot escape it; if it cannot glob, it knows "
                     "nothing, and an unchecked structural guarantee is not a held one\n");
        return false;
    }

    std::vector<std::string> candidates;
    {
        std::error_code ec;
        std::filesystem::directory_iterator it(dir, ec);
        if (ec) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: could not enumerate %s (%s), so the set of "
                         "traversal instantiations is unknown\n",
                         dir.c_str(), ec.message().c_str());
            return false;
        }
        for (const std::filesystem::directory_entry& e : it) {
            if (!e.is_regular_file()) continue;
            if (e.path().extension() != ".comp") continue;
            candidates.push_back("shaders/diff/" + e.path().filename().string());
        }
    }
    // Sorted so the diagnostics and the NOTE below are deterministic on a
    // filesystem that does not enumerate in a stable order.
    std::sort(candidates.begin(), candidates.end());
    if (candidates.empty()) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s contains no `.comp` file at all. The scatter stage "
                     "is one of them, so either the directory found is not the one this probe "
                     "compiles from or the shaders are gone\n",
                     dir.c_str());
        return false;
    }

    struct Instantiation {
        std::string path;
        const char* role;
        std::string stripped;
        std::string found;
    };
    std::vector<Instantiation> instantiations;
    for (const std::string& rel : candidates) {
        std::string stripped, found;
        if (!loadShaderSourceStripped(rel.c_str(), stripped, found)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %s was enumerated in %s but could not then be "
                         "opened through the loader, so this check cannot tell whether it includes "
                         "the shared traversal. A file it cannot read is a file it cannot clear\n",
                         rel.c_str(), dir.c_str());
            return false;
        }
        // Matched on the COMMENT-STRIPPED text, so a commented-out include
        // does not enroll a file -- and, far more to the point, so a real one
        // cannot hide behind a reader's assumption that only two files matter.
        const std::regex traverseIncludeRe(
            R"RX(#[ \t]*include[ \t]*"diff/traverse\.glsl")RX");
        if (!std::regex_search(stripped, traverseIncludeRe)) continue;
        const char* role = "an ADDITIONAL instantiation, discovered by glob";
        if (rel == "shaders/diff/wf_scatter.comp") role = "the FORWARD instantiation";
        if (rel == "shaders/diff/wf_scatter_replay.comp") role = "the REPLAY instantiation";
        instantiations.push_back({rel, role, std::move(stripped), std::move(found)});
    }

    if (instantiations.size() < 2u) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: only %zu file(s) in %s include "
                     "\"diff/traverse.glsl\". Stage 1 rests on the traversal having TWO "
                     "instantiations -- a forward one and a replay one -- that walk the same path; "
                     "with fewer than two there is nothing for the replay-equivalence check to "
                     "compare, and deleting an instantiation must not be a way to pass this "
                     "check\n",
                     instantiations.size(), dir.c_str());
        return false;
    }

    std::string sharedVersion;
    std::string sharedVersionFile;

    for (const Instantiation& inst : instantiations) {
        const std::string& stripped = inst.stripped;
        const std::string& found = inst.found;

        // (1) exactly one #include, and it is the traversal.
        // Custom raw-string delimiter: the pattern itself contains `)"`.
        const std::regex includeRe(R"RX(#[ \t]*include[ \t]*"([^"]*)")RX");
        std::vector<std::string> includes;
        for (auto it = std::sregex_iterator(stripped.begin(), stripped.end(), includeRe);
             it != std::sregex_iterator(); ++it) {
            includes.push_back((*it)[1].str());
        }
        if (includes.size() != 1 || includes[0] != "diff/traverse.glsl") {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %s (%s) has %zu #include(s)%s%s, expected exactly "
                         "one of \"diff/traverse.glsl\". Anything else this file pulls in is text "
                         "the OTHER instantiation does not compile, which is precisely the "
                         "divergence spec 6.2 removes by construction\n",
                         found.c_str(), inst.role, includes.size(),
                         includes.empty() ? "" : ", first is \"",
                         includes.empty() ? "" : (includes[0] + "\"").c_str());
            return false;
        }

        // (2) no layout() declarations.
        if (stripped.find("layout") != std::string::npos) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %s (%s) contains a `layout` declaration. Every "
                         "binding, the Push block and the local size belong to "
                         "shaders/includes/diff/traverse.glsl so that BOTH instantiations have "
                         "exactly one of each. A layout here is a descriptor interface -- or a "
                         "Push block, byte-matched to WavefrontLoop::ScatterPush -- that only one "
                         "of the two kernels has\n",
                         found.c_str(), inst.role);
            return false;
        }

        // (5) no preprocessor directive except #version and the #include.
        const std::regex directiveRe(R"((?:^|\n)[ \t]*#[ \t]*([A-Za-z_]+))");
        for (auto it = std::sregex_iterator(stripped.begin(), stripped.end(), directiveRe);
             it != std::sregex_iterator(); ++it) {
            const std::string d = (*it)[1].str();
            if (d == "version" || d == "include") continue;
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %s (%s) contains the preprocessor directive "
                         "`#%s`. Only `#version` and the one `#include` are allowed: a `#define` "
                         "here plus an `#ifdef` in the shared traversal is textually one source "
                         "and behaviourally two, and one extra `diffRngNext1D` behind such a "
                         "switch is exactly the silent divergence this whole task exists to make "
                         "impossible\n",
                         found.c_str(), inst.role, d.c_str());
            return false;
        }

        // (3) top-level function definitions.
        const std::regex fnRe(
            R"((?:^|\n)([A-Za-z_][A-Za-z0-9_]*)[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*\()");
        std::set<std::string> defined;
        for (auto it = std::sregex_iterator(stripped.begin(), stripped.end(), fnRe);
             it != std::sregex_iterator(); ++it) {
            defined.insert((*it)[2].str());
        }
        const std::set<std::string> kAllowed = {"diffVertexHook", "main"};
        if (defined != kAllowed) {
            std::string listed;
            for (const std::string& n : defined) {
                if (!listed.empty()) listed += ", ";
                listed += n;
            }
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %s (%s) defines top-level function(s) {%s}, "
                         "expected exactly {diffVertexHook, main}. The hook is the ONLY thing an "
                         "instantiation contributes; a second function defined here is a second "
                         "traversal the other instantiation does not have\n",
                         found.c_str(), inst.role, listed.c_str());
            return false;
        }

        // (4) main's body.
        const std::regex mainRe(
            R"(void[ \t]+main[ \t]*\([ \t]*\)[ \t]*\{[ \t\r\n]*diffTraverse\([ \t]*\)[ \t]*;[ \t\r\n]*\})");
        if (!std::regex_search(stripped, mainRe)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %s (%s) does not spell its entry point as "
                         "`void main() { diffTraverse(); }`. Work done before or after the shared "
                         "traversal is work only one of the two kernels does\n",
                         found.c_str(), inst.role);
            return false;
        }

        // (6) the #version, compared rather than merely permitted. Rule 5
        // lets `#version` through because a `.comp` must have one; letting it
        // through WITHOUT comparing the numbers left the instantiations free
        // to compile the one shared source under two language versions, which
        // is a behavioural difference inside a single source -- exactly the
        // shape rule 5 exists to exclude. The profile token is captured too:
        // `460 core` and `460 compatibility` are not the same language.
        const std::regex versionRe(
            R"((?:^|\n)[ \t]*#[ \t]*version[ \t]+([0-9]+)[ \t]*([A-Za-z_]*))");
        std::smatch vm;
        if (!std::regex_search(stripped, vm, versionRe)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %s (%s) declares no `#version`, so the language "
                         "version it compiles the shared traversal under cannot be compared with "
                         "the other instantiations'\n",
                         found.c_str(), inst.role);
            return false;
        }
        const std::string version = vm[1].str() + (vm[2].str().empty() ? "" : " " + vm[2].str());
        if (sharedVersion.empty()) {
            sharedVersion = version;
            sharedVersionFile = found;
        } else if (version != sharedVersion) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %s (%s) declares `#version %s` while %s declares "
                         "`#version %s`. Two instantiations of ONE traversal compiled under two "
                         "GLSL versions are two behaviours in one source -- the same divergence a "
                         "`#define` would buy, through the one directive rule 5 has to allow\n",
                         found.c_str(), inst.role, version.c_str(), sharedVersionFile.c_str(),
                         sharedVersion.c_str());
            return false;
        }
    }

    std::string listed;
    for (const Instantiation& inst : instantiations) {
        if (!listed.empty()) listed += ", ";
        listed += inst.path;
    }
    std::printf("[diff_gpu_probe] NOTE: one traversal, %zu instantiations -- every `.comp` in %s "
                "that includes shaders/includes/diff/traverse.glsl was DISCOVERED by glob (a "
                "third one cannot escape this check by not being listed in it), and all of them "
                "{%s} include that file and nothing else, declare no layout() of their own, carry "
                "no preprocessor switch, define exactly {diffVertexHook, main}, spell main as "
                "`void main() { diffTraverse(); }`, and agree on `#version %s`. The only "
                "difference between them is the hook body\n",
                instantiations.size(), dir.c_str(), listed.c_str(), sharedVersion.c_str());
    return true;
}

/// Ties wf_scatter.comp's `Push` block byte size to
/// `ohao::diff::WavefrontLoop::ScatterPush`'s, the same way checkNeeStrideTie
/// ties the NEE record stride: by parsing the SHADER SOURCE (comment-stripped
/// via loadDiffTraverseSourceStripped, for the identical reason) rather than
/// trusting a comment.
///
/// WHY THIS EXISTS (review Finding 6, Stage 0b-2b Task 5). ScatterPush has no
/// static_assert and no runtime tie today, and it has grown a tail field in
/// each of Tasks 2 (material), 3 (environment) and 5 (film) -- three chances
/// for the C++ struct and the GLSL block to drift, caught so far only by two
/// humans reading two files side by side. kNeeSampleFloats got a runtime tie
/// precisely because "naming each other in a comment was not enough"; the
/// same argument applies here with equal force, so this reuses that
/// mechanism rather than settling for a static_assert against a hand-copied
/// literal, which would tie ScatterPush to a DOCUMENTED number but not to the
/// shader itself -- it would not notice a field added to the GLSL block
/// without a matching C++ change, only the reverse.
///
/// WHAT IT ACTUALLY COVERS, AND WHERE IT IS NOT STRONGER (review Finding 6,
/// Task 5 fix -- this doc corrects an earlier version of itself). The
/// field-matching regex ALREADY CAPTURED EACH FIELD'S NAME to count them; it
/// just was not comparing them. It now is: the captured name list is compared
/// IN ORDER against a canonical list held here (`kCanonicalFieldOrder`), so
/// this ties field IDENTITY and ORDER, not merely COUNT -- a reorder within
/// the block (a `scaleU`/`scaleV` swap, say) now fails here even though both
/// fields are the same width and the byte count does not move. What it still
/// does NOT tie is field TYPE: every field is a 4-byte scalar today, so a
/// `uint` swapped for a `float` in place changes neither the name list nor
/// the byte count and passes here -- a real wrong-value push (a float pushed
/// where an integer bit-pattern is read, or vice versa) this check cannot
/// see. It does fail closed in every other degradation path it can see: an
/// unopenable source, a renamed Push block, a missing `} pc;`, and a
/// non-scalar field (caught by the semicolon cross-check) are all hard
/// failures rather than silent passes.
///
/// HOW THE BYTE COUNT IS COMPUTED. Every field the Push block has ever had is
/// a bare scalar `uint` or `float` -- no vec/mat/array member exists in it --
/// so each one is 4 bytes with no interior padding, and the block's size is
/// simply 4 * (field count). That assumption is exactly the kind of thing
/// that could silently stop holding, so it is checked rather than baked in:
/// the field-matching regex's count is cross-checked against a plain
/// semicolon count over the same block, and a mismatch (e.g. a vec3 field,
/// which this regex does not match) fails loudly instead of mis-sizing.
bool checkScatterPushSizeTie() {
    std::string stripped, found;
    if (!loadDiffTraverseSourceStripped(stripped, found)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: could not open shaders/includes/diff/traverse.glsl from any "
                     "candidate path, so its Push block size could not be tied to "
                     "ohao::diff::WavefrontLoop::ScatterPush. An unchecked tie is not a held "
                     "tie\n");
        return false;
    }

    const std::string beginMarker = "layout(push_constant) uniform Push {";
    const std::size_t beginPos = stripped.find(beginMarker);
    if (beginPos == std::string::npos) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s no longer declares `layout(push_constant) uniform "
                     "Push { ... } pc;` in the exact spelling this check looks for, so its size "
                     "cannot be tied to ScatterPush. Restore the spelling or update this check -- "
                     "do not leave the two untied\n",
                     found.c_str());
        return false;
    }
    const std::size_t blockStart = beginPos + beginMarker.size();
    const std::size_t endPos = stripped.find("} pc;", blockStart);
    if (endPos == std::string::npos) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s's Push block has no matching `} pc;` this check "
                     "can find, so its size cannot be tied to ScatterPush\n",
                     found.c_str());
        return false;
    }
    const std::string block = stripped.substr(blockStart, endPos - blockStart);

    // Captures each field's NAME (group 1), not merely its presence -- this
    // is what lets the order tie below compare identities, not just count.
    const std::regex fieldRe(R"((?:uint|float)[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*;)");
    std::vector<std::string> fieldNames;
    for (auto it = std::sregex_iterator(block.begin(), block.end(), fieldRe);
         it != std::sregex_iterator(); ++it) {
        fieldNames.push_back((*it)[1].str());
    }
    const std::size_t fieldCount = fieldNames.size();
    const std::size_t semicolons =
        static_cast<std::size_t>(std::count(block.begin(), block.end(), ';'));
    if (semicolons != fieldCount || fieldCount == 0) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s's Push block has %zu statement(s) but only %zu "
                     "matched a bare `uint`/`float` scalar field. This check's byte-size math (4 "
                     "bytes/field, no padding) assumes every field is one of those two scalar "
                     "types; a vec/array/other-typed field would silently mis-size instead of "
                     "being counted correctly. Update this check to handle it rather than "
                     "trusting the mismatch away\n",
                     found.c_str(), semicolons, fieldCount);
        return false;
    }

    const std::size_t shaderBytes = fieldCount * 4u;
    constexpr std::size_t kScatterPushBytes = sizeof(ohao::diff::WavefrontLoop::ScatterPush);
    if (shaderBytes != kScatterPushBytes) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s's Push block is %zu scalar fields (%zu bytes) but "
                     "sizeof(ohao::diff::WavefrontLoop::ScatterPush) is %zu bytes. "
                     "WavefrontLoop::record fills that struct and vkCmdPushConstants pushes it "
                     "byte-for-byte as this shader's push constants; a size mismatch is a silent "
                     "wrong-field push, not a validation error, and every field after the point "
                     "of drift is read at the wrong offset\n",
                     found.c_str(), fieldCount, shaderBytes, kScatterPushBytes);
        return false;
    }

    // --- THE ORDER TIE (review Finding 6, Task 5 fix). The regex above
    // already captures every field's name; comparing that captured list, IN
    // ORDER, against a canonical transcription of ScatterPush's own
    // declaration order closes the gap the byte-size tie alone leaves open --
    // a reorder within the block (same fields, same count, same total bytes,
    // different offsets) now fails here instead of passing silently. This
    // canonical list is a hand-kept transcription of
    // `ohao::diff::WavefrontLoop::ScatterPush`'s member order (like
    // `kCanonicalRhs` in `checkTexelOrderingTie` is a hand-kept transcription
    // of the shader's ordering formula): update it in the same commit that
    // reorders, renames, adds or removes a ScatterPush field.
    static const std::vector<std::string> kCanonicalFieldOrder = {
        "capacity",          "srcQueueBase",       "srcCountSlot",
        "dstQueueBase",      "dstCountSlot",       "albedo",
        "iterationSeed",     "roughness",          "metallic",
        "specularWeight",    "envWidth",           "envHeight",
        "envIntegral",       "filmPixelCount",     "gradArenaFloats",
        "gradAlbedoOffset",  "sampleAlbedo",       "sampleRoughness",
        "sampleMetallic",    "sampleSpecularWeight", "diffParam",
        "emission",          "emissionTexWidth",   "emissionTexHeight",
        "emissionTexChannels", "emissionUvScaleU", "emissionUvScaleV",
        "emissionUvBiasU",   "emissionUvBiasV",   "adjointSeedFloats",
    };
    if (fieldNames != kCanonicalFieldOrder) {
        const std::size_t n = std::min(fieldNames.size(), kCanonicalFieldOrder.size());
        std::size_t firstDiff = n;
        for (std::size_t i = 0; i < n; ++i) {
            if (fieldNames[i] != kCanonicalFieldOrder[i]) {
                firstDiff = i;
                break;
            }
        }
        std::string got = (firstDiff < fieldNames.size()) ? fieldNames[firstDiff] : "<missing>";
        std::string want = (firstDiff < kCanonicalFieldOrder.size()) ? kCanonicalFieldOrder[firstDiff]
                                                                      : "<missing>";
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s's Push block and "
                     "ohao::diff::WavefrontLoop::ScatterPush agree on field COUNT (%zu) and byte "
                     "size (%zu) but not on field ORDER: field %zu is `%s` in the shader and `%s` "
                     "in the canonical order this check holds. Both structs are 4-byte scalars "
                     "throughout, so a reorder does not change the count or the byte size -- only "
                     "which value lands at which offset. WavefrontLoop::record fills ScatterPush "
                     "by NAME and vkCmdPushConstants pushes it byte-for-byte, so a drift here is a "
                     "silent wrong-field push the size tie alone cannot see\n",
                     found.c_str(), fieldCount, shaderBytes, firstDiff, got.c_str(), want.c_str());
        return false;
    }

    std::printf("[diff_gpu_probe] NOTE: the scatter traversal's Push block tied to ScatterPush -- %s "
                "declares %zu scalar fields (%zu bytes), in the same order as ScatterPush, and "
                "sizeof(ScatterPush) is %zu\n",
                found.c_str(), fieldCount, shaderBytes, kScatterPushBytes);
    return true;
}

/// Ties the two remaining shader constants this probe transcribes by hand:
/// bsdf_probe.comp's output stride (against kBsdfProbeFloatsPerCase, which
/// check 20 strides its readback by) and bsdf.glsl's DIFF_BSDF_MIN_COS
/// (against kShaderGrazingCos).
///
/// WHY THE SECOND ONE MATTERS MOST (review finding). kShaderGrazingCos is not
/// used to ASSERT anything -- it is used to EXCUSE. Check 20's sampler-weight
/// comparison skips a case whose cosine falls at or below it, on the stated
/// grounds that the shader refuses the specular math there and the CPU oracle
/// does not. If bsdf.glsl's floor rose and this constant did not, check 20
/// would go on excusing rejections that were no longer explained by the
/// documented disagreement -- an excuse widening silently is strictly worse
/// than an assertion loosening silently, because nothing in the output would
/// change. So it is tied to the source, like every other constant this probe
/// mirrors.
bool checkBsdfShaderConstantTies() {
    std::string probeSrc, probePath;
    if (!loadShaderSourceStripped("shaders/diff/bsdf_probe.comp", probeSrc, probePath)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: could not open shaders/diff/bsdf_probe.comp from any "
                     "candidate path, so check 20's readback stride could not be tied to that "
                     "shader's own output stride. An unchecked tie is not a held tie\n");
        return false;
    }
    std::string bsdfSrc, bsdfPath;
    if (!loadShaderSourceStripped("shaders/includes/diff/bsdf.glsl", bsdfSrc, bsdfPath)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: could not open shaders/includes/diff/bsdf.glsl from "
                     "any candidate path, so kShaderGrazingCos could not be tied to "
                     "DIFF_BSDF_MIN_COS. An unchecked tie is not a held tie\n");
        return false;
    }

    const std::regex strideDecl(
        R"(const[ \t]+uint[ \t]+base[ \t]*=[ \t]*pc\.outIndex[ \t]*\*[ \t]*([0-9]+)u[ \t]*;)");
    std::smatch mStride;
    if (!std::regex_search(probeSrc, mStride, strideDecl)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s no longer computes its output base as `const uint "
                     "base = pc.outIndex * <N>u;`, so check 20's readback stride cannot be tied "
                     "to it. Restore the spelling or update this check -- do not leave the two "
                     "untied\n",
                     probePath.c_str());
        return false;
    }
    const unsigned long shaderStride = std::stoul(mStride[1].str());
    if (shaderStride != static_cast<unsigned long>(kBsdfProbeFloatsPerCase)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s writes %lu floats per case while check 20 strides "
                     "its readback by %u. Every case past the first would be read at the wrong "
                     "offset, and the BSDF comparison would be measuring the wrong floats\n",
                     probePath.c_str(), shaderStride, kBsdfProbeFloatsPerCase);
        return false;
    }

    const std::regex outWrite(
        R"(outBuf\.v\[[ \t]*base[ \t]*\+[ \t]*([0-9]+)u[ \t]*\][ \t]*=)");
    std::set<std::uint32_t> offsets;
    for (auto it = std::sregex_iterator(probeSrc.begin(), probeSrc.end(), outWrite);
         it != std::sregex_iterator(); ++it) {
        offsets.insert(static_cast<std::uint32_t>(std::stoul((*it)[1].str())));
    }
    if (offsets.size() != kBsdfProbeFloatsPerCase || *offsets.begin() != 0u ||
        *offsets.rbegin() != kBsdfProbeFloatsPerCase - 1u) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s writes %zu distinct offsets into its %u-float "
                     "output record. A slot the shader never writes is read back by check 20 as "
                     "whatever the arena happened to hold\n",
                     probePath.c_str(), offsets.size(), kBsdfProbeFloatsPerCase);
        return false;
    }

    const std::regex minCosDecl(
        R"(const[ \t]+float[ \t]+DIFF_BSDF_MIN_COS[ \t]*=[ \t]*([0-9.eE+-]+)[ \t]*;)");
    std::smatch mMinCos;
    if (!std::regex_search(bsdfSrc, mMinCos, minCosDecl)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s no longer declares `const float DIFF_BSDF_MIN_COS "
                     "= <N>;` on one line, so check 20's grazing-rejection excuse cannot be tied "
                     "to the threshold it excuses. Restore the spelling or update this check\n",
                     bsdfPath.c_str());
        return false;
    }
    const double shaderMinCos = std::stod(mMinCos[1].str());
    if (shaderMinCos != kShaderGrazingCos) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s declares DIFF_BSDF_MIN_COS = %.9g but this probe "
                     "uses kShaderGrazingCos = %.9g to EXCUSE check 20's grazing rejections. With "
                     "the two apart, check 20 would keep excusing shader rejections the "
                     "documented disagreement no longer explains -- and would print nothing "
                     "different while doing it\n",
                     bsdfPath.c_str(), shaderMinCos, kShaderGrazingCos);
        return false;
    }

    std::printf("[diff_gpu_probe] NOTE: BSDF shader constants tied -- %s writes %lu floats per "
                "case at offsets 0..%u (check 20's stride is %u) and %s declares "
                "DIFF_BSDF_MIN_COS = %.9g, matching kShaderGrazingCos, the constant check 20 uses "
                "to excuse a grazing rejection\n",
                probePath.c_str(), shaderStride, kBsdfProbeFloatsPerCase - 1u,
                kBsdfProbeFloatsPerCase, bsdfPath.c_str(), shaderMinCos);
    return true;
}

/// Ties kParityRayTMax and kParitySurfaceOffset -- the two constants
/// checks 33-34's CPU reference (ParityRefScene) derives its rayTMax and
/// surfaceOffset from -- to the SHADER constants they mirror, by parsing the
/// comment-stripped sources loadShaderSourceStripped loads. Before this
/// check existed, the pairing was a COMMENT naming the shader constants next
/// to two C++ literals -- exactly the pattern this branch built
/// checkNeeStrideTie and checkScatterPushSizeTie to replace, because a
/// comment is not a tie. Severity was judged low (drift here fails LOUDLY:
/// the reference becomes wrong and checks 33-34 reject, rather than silently
/// reading garbage the way a stride mismatch would), but the mechanism to
/// check it exists and reusing it is cheap, so it is checked rather than
/// left to a comment.
///
/// WHAT IS COVERED, EXACTLY -- because an earlier version of this check
/// covered less than its own message claimed (review finding). kParityRayTMax
/// feeds TWO reference functions with two different shader counterparts:
///
///   * parityOccluded, the SHADOW ray, mirrors wf_scatter.comp's
///     `kShadowTMax`;
///   * parityTraceNearest, the PRIMARY and CONTINUATION trace, mirrors
///     wf_intersect.comp's `kTraceTMax`.
///
/// Only the first was tied. wf_intersect.comp wrote its tMax as a bare
/// literal inside the rayQueryInitializeEXT call, so there was no name to
/// search for and nothing tied it: changing it alone would have left this
/// check still printing "matching ... exactly" while the reference's escape
/// semantics for PATH rays no longer matched the shader's. That literal is
/// now `const float kTraceTMax` and is parsed here, so BOTH counterparts of
/// kParityRayTMax are tied and both must equal it.
///
/// kParitySurfaceOffset has exactly ONE shader counterpart,
/// wf_scatter.comp's `kSurfaceOffset` -- the reference applies it to both the
/// shadow ray's origin and the continuation ray's because that shader derives
/// both from that one constant. wf_intersect.comp contributes NOTHING to it:
/// it deliberately traces with tMin = 0 and no offset of its own (see the
/// derivation at the head of its ray query). It has no `1e-4` constant of its
/// own -- the only occurrence of that string is the comment at :158 saying why
/// there is none, which is the opposite of a value this reference mirrors.
bool checkParityRefConstantsTie() {
    std::string scatterSrc, scatterPath;
    if (!loadDiffTraverseSourceStripped(scatterSrc, scatterPath)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: could not open shaders/includes/diff/traverse.glsl from any "
                     "candidate path, so checks 33-34's CPU reference rayTMax/surfaceOffset could "
                     "not be tied to wf_scatter.comp's kShadowTMax/kSurfaceOffset. An unchecked "
                     "tie is not a held tie\n");
        return false;
    }
    std::string intersectSrc, intersectPath;
    if (!loadShaderSourceStripped("shaders/diff/wf_intersect.comp", intersectSrc, intersectPath)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: could not open shaders/diff/wf_intersect.comp from "
                     "any candidate path, so checks 33-34's CPU reference rayTMax could not be "
                     "tied to the PATH ray's tMax (kTraceTMax). An unchecked tie is not a held "
                     "tie\n");
        return false;
    }

    const std::regex shadowTMaxDecl(
        R"(const[ \t]+float[ \t]+kShadowTMax[ \t]*=[ \t]*([0-9.eE+-]+)[ \t]*;)");
    const std::regex offsetDecl(
        R"(const[ \t]+float[ \t]+kSurfaceOffset[ \t]*=[ \t]*([0-9.eE+-]+)[ \t]*;)");
    const std::regex traceTMaxDecl(
        R"(const[ \t]+float[ \t]+kTraceTMax[ \t]*=[ \t]*([0-9.eE+-]+)[ \t]*;)");
    std::smatch mShadowTMax, mOffset, mTraceTMax;
    if (!std::regex_search(scatterSrc, mShadowTMax, shadowTMaxDecl) ||
        !std::regex_search(scatterSrc, mOffset, offsetDecl)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s no longer declares `const float kShadowTMax = "
                     "<N>;` and `const float kSurfaceOffset = <N>;` on one line each, so checks "
                     "33-34's CPU reference constants cannot be tied to the shader's. Restore the "
                     "spelling or update this check -- do not leave the two constants untied\n",
                     scatterPath.c_str());
        return false;
    }
    if (!std::regex_search(intersectSrc, mTraceTMax, traceTMaxDecl)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s no longer declares `const float kTraceTMax = "
                     "<N>;` on one line, so the PATH ray's tMax cannot be tied to checks 33-34's "
                     "kParityRayTMax. It was a bare literal inside rayQueryInitializeEXT once, "
                     "and that is exactly the state this check exists to prevent returning to -- "
                     "restore the name or update this check, do not leave it untied\n",
                     intersectPath.c_str());
        return false;
    }
    const double shaderShadowTMax = std::stod(mShadowTMax[1].str());
    const double shaderOffset = std::stod(mOffset[1].str());
    const double shaderTraceTMax = std::stod(mTraceTMax[1].str());
    if (shaderShadowTMax != kParityRayTMax || shaderTraceTMax != kParityRayTMax ||
        shaderOffset != kParitySurfaceOffset) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s declares kShadowTMax = %.9g and kSurfaceOffset = "
                     "%.9g, %s declares kTraceTMax = %.9g, but checks 33-34's CPU reference uses "
                     "kParityRayTMax = %.9g (for BOTH its shadow ray and its path ray) and "
                     "kParitySurfaceOffset = %.9g. The reference's escape/occlusion semantics or "
                     "ray-offset epsilon no longer match the shader's, so a pass on checks 33-34 "
                     "would not be evidence of parity with THESE shaders\n",
                     scatterPath.c_str(), shaderShadowTMax, shaderOffset, intersectPath.c_str(),
                     shaderTraceTMax, kParityRayTMax, kParitySurfaceOffset);
        return false;
    }
    std::printf("[diff_gpu_probe] NOTE: checks 33-34's CPU reference constants tied -- %s declares "
                "kShadowTMax = %.9g (the reference's SHADOW ray) and kSurfaceOffset = %.9g, %s "
                "declares kTraceTMax = %.9g (the reference's PRIMARY and CONTINUATION ray), all "
                "matching kParityRayTMax = %.9g and kParitySurfaceOffset = %.9g exactly\n",
                scatterPath.c_str(), shaderShadowTMax, shaderOffset, intersectPath.c_str(),
                shaderTraceTMax, kParityRayTMax, kParitySurfaceOffset);
    return true;
}

/// THE TEXEL ELEMENT ORDERING, tied on BOTH sides (Stage 1 Task 5).
///
/// WHAT WAS UNTIED BEFORE THIS. `ParamShape::floatCount()` is w*h*c -- a
/// COUNT. A count implies no ordering whatsoever: row-major, column-major and
/// channel-planar all reserve exactly the same number of floats. Stage 1 Task
/// 2 wrote `k = (y*width + x)*channels + c` into traverse.glsl's arena comment
/// as "the ordering floatCount() already implies", then correctly withdrew the
/// claim and left the convention UNESTABLISHED, on the grounds that a
/// host-side tie belongs to the task that first has a texture to tie and that
/// "inventing one here would be a claim nothing checks". Until this function
/// existed the mapping from (x, y, c) to an arena offset was asserted nowhere
/// and tested nowhere, on either side of the GLSL/C++ boundary.
///
/// WHY IT IS A CHECK AND NOT A COMMENT. A disagreement about texel order is
/// not a crash and not a validation error: it is a SILENT WRONG-SLOT SCATTER.
/// Every gradient still lands inside the parameter's own block, the block's
/// total is still right (the conservation identity does not care which slot
/// got what), and the optimizer still descends -- into a transposed image.
/// That is the same failure class `checkNeeStrideTie`, `checkScatterPushSizeTie`,
/// `checkWfScatterSinkLayoutTie` and `checkParityRefConstantsTie` exist for,
/// and it gets the same treatment: the probe REFUSES TO RUN if the two sides
/// disagree.
///
/// HOW IT TIES, in two independent halves, because one alone would not do:
///
///   1. THE SHADER'S SPELLING. `shaders/includes/diff/bsdf_adjoint.glsl`'s
///      `diffTexelElementIndex` is parsed out of the SOURCE -- its parameter
///      list and the right-hand side of its single `return` -- and both must
///      match the canonical spelling held here. This is what catches the
///      shader changing alone.
///   2. THE C++ FUNCTION'S SEMANTICS. `ParamShape::elementIndex` is EVALUATED
///      at every (x, y, c) of a non-degenerate, NON-SQUARE, multi-channel
///      shape and must agree with the canonical formula at every one of them.
///      THIS EXHAUSTIVE EQUALITY -- against the literal `kCanonicalRhs`, at
///      all 36 triples -- is what pins the ordering, not the bijection check
///      below it: a BIJECTION IS NOT SUFFICIENT, since the transposed
///      ordering `(x*height + y)*channels + c` is also a bijection onto the
///      same 36 floats and would be accepted by a bijection test alone. The
///      resulting indices are additionally required to form a BIJECTION onto
///      [0, floatCount()) -- a genuine EXTRA check (it catches a collision or
///      a gap the pointwise equality above would not, were the canonical
///      formula itself somehow non-bijective), not the half that does the
///      pinning.
///
/// The shape is 4x3x3 deliberately. A SQUARE shape hides a row/column
/// transposition (both orderings agree); a SINGLE-CHANNEL shape hides the
/// `* channels + c` factor entirely (channel-interleaved and channel-planar
/// agree when there is one channel). Neither degeneracy can hide here.
///
/// WHAT THIS DOES NOT TIE, stated so nobody infers more from it: that the
/// SHADER computes what its source says. That is measured on the GPU, by
/// check 44, which predicts from `ParamShape::elementIndex` ALONE which arena
/// floats a known bilinear footprint may touch and requires every other float
/// of the whole arena to be exactly 0.
bool checkTexelOrderingTie() {
    // The canonical spelling, held HERE and compared against both sides. It
    // is deliberately a literal and not a call to either function: a tie
    // whose reference is one of the two things it ties is not a tie.
    static const char* const kCanonicalRhs = "(y * width + x) * channels + c";
    static const char* const kCanonicalParams = "uint width, uint channels, uint x, uint y, uint c";

    std::string stripped, found;
    if (!loadShaderSourceStripped("shaders/includes/diff/bsdf_adjoint.glsl", stripped, found)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: could not open shaders/includes/diff/bsdf_adjoint.glsl "
                     "from any candidate path, so the texture element ordering could not be tied "
                     "to ohao::diff::ParamShape::elementIndex. An unchecked tie is not a held "
                     "tie: a GLSL/C++ disagreement here is a silent wrong-slot scatter, not a "
                     "validation error\n");
        return false;
    }

    const std::regex decl(
        R"(uint[ \t\r\n]+diffTexelElementIndex[ \t\r\n]*\(([^)]*)\)[ \t\r\n]*\{[ \t\r\n]*return([^;]*);)");
    std::smatch m;
    if (!std::regex_search(stripped, m, decl)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s no longer defines `uint diffTexelElementIndex(...) "
                     "{ return <expr>; }` in a form this check can parse, so the texture element "
                     "ordering cannot be tied to ohao::diff::ParamShape::elementIndex. Restore "
                     "the spelling or update this check -- do not leave the two untied\n",
                     found.c_str());
        return false;
    }
    const std::string shaderParams = squashWhitespace(m[1].str());
    const std::string shaderRhs = squashWhitespace(m[2].str());
    if (shaderParams != kCanonicalParams || shaderRhs != kCanonicalRhs) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s's diffTexelElementIndex is\n"
                     "    (%s) -> %s\n"
                     "  and the ordering ohao::diff::ParamShape::elementIndex implements is\n"
                     "    (%s) -> %s\n"
                     "  A texture parameter's primal array (binding 11) and its gradient block "
                     "are addressed by the SHADER's expression and uploaded/read back by the "
                     "HOST's. Disagreeing about which float is texel (x, y) channel c is a silent "
                     "wrong-slot scatter: the block total stays right, every check that reads only "
                     "totals stays green, and the gradient image comes out transposed\n",
                     found.c_str(), shaderParams.c_str(), shaderRhs.c_str(), kCanonicalParams,
                     kCanonicalRhs);
        return false;
    }

    // --- Half 2: the C++ side, EVALUATED rather than read.
    const ohao::diff::ParamShape shape{4u, 3u, 3u};
    const std::uint32_t floats = shape.floatCount();
    if (floats != 36u) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: ParamShape{4,3,3}.floatCount() is %u, expected 36. "
                     "The ordering below is checked to be a bijection onto [0, floatCount()), "
                     "which says nothing if the count itself is wrong\n",
                     floats);
        return false;
    }
    std::vector<int> hits(floats, 0);
    for (std::uint32_t y = 0; y < shape.height; ++y) {
        for (std::uint32_t x = 0; x < shape.width; ++x) {
            for (std::uint32_t c = 0; c < shape.channels; ++c) {
                const std::uint32_t k = shape.elementIndex(x, y, c);
                const std::uint32_t canonical = (y * shape.width + x) * shape.channels + c;
                if (k != canonical) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: ParamShape{4,3,3}.elementIndex(%u, %u, "
                                 "%u) is %u; the canonical row-major, channel-interleaved "
                                 "ordering `%s` gives %u. The SHADER spells that canonical form "
                                 "(checked above), so the two sides now disagree and every "
                                 "gradient this probe reads back would be attributed to the wrong "
                                 "texel\n",
                                 x, y, c, k, kCanonicalRhs, canonical);
                    return false;
                }
                if (k >= floats) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: ParamShape{4,3,3}.elementIndex(%u, %u, "
                                 "%u) is %u, outside the %u floats floatCount() reserves. An "
                                 "ordering that leaves its own block is a write past the end of "
                                 "the parameter\n",
                                 x, y, c, k, floats);
                    return false;
                }
                ++hits[k];
            }
        }
    }
    for (std::uint32_t k = 0; k < floats; ++k) {
        if (hits[k] != 1) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: float %u of a ParamShape{4,3,3} block is named by "
                         "%d distinct (x, y, c) triples, not exactly 1. The ordering must be a "
                         "BIJECTION onto [0, floatCount()): a collision means two texels share "
                         "one gradient float and a gap means a texel's gradient is never "
                         "written\n",
                         k, hits[k]);
            return false;
        }
    }

    std::printf("[diff_gpu_probe] NOTE: texture element ordering tied -- %s spells "
                "diffTexelElementIndex(%s) -> %s, and ohao::diff::ParamShape::elementIndex agrees "
                "with it at all %u (x, y, c) of a 4x3x3 (non-square, multi-channel) shape, "
                "bijectively onto [0, %u)\n",
                found.c_str(), kCanonicalParams, kCanonicalRhs, floats, floats);
    return true;
}

}  // namespace ohao::diff::probe
