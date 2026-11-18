#pragma once
// Jolt's jobs on the engine's job pool (step 4b, "Jolt on the pool"): JoltJobs is a JPH::JobSystemWithBarrier whose
// jobs are frame tasks. Jolt is built without RTTI, so the class lives in jolt_jobs.cpp (compiled -fno-rtti) and
// this header only names it.
//
// - Jolt counts its jobs' dependencies; a job whose last dependency finished is queued as a pool task. The thread
//   in PhysicsSystem::Update waits on Jolt's barrier, which runs the barrier's ready jobs itself meanwhile, then
//   on the tasks it queued (finishJoltJobs).
// - Inline (a small world): GetMaxConcurrency is 1 and nothing is queued, so the barrier runs every job on the
//   calling thread. Jolt's result does not depend on its concurrency, so this changes the time, never the state.

#if !defined(OHAO_HAS_JOLT) || OHAO_HAS_JOLT

#include <memory>

namespace JPH {
class JobSystem;
}

namespace ohao::physics::backend {

class JoltJobs;
struct JoltJobsDeleter {
    void operator()(JoltJobs* jobs) const noexcept;
};
using JoltJobsPtr = std::unique_ptr<JoltJobs, JoltJobsDeleter>;

[[nodiscard]] JoltJobsPtr makeJoltJobs();
// The JPH::JobSystem to pass to PhysicsSystem::Update.
[[nodiscard]] JPH::JobSystem* joltJobSystem(JoltJobs& jobs) noexcept;
// Inline or on the pool, for the next Update (never during one).
void setJoltJobsInline(JoltJobs& jobs, bool inlineJobs) noexcept;
// After an Update: returns once every pool task it queued has run. A job the barrier ran itself leaves its task
// behind as a no-op still holding the job; without this wait that task could outlive the step, or the backend.
void finishJoltJobs(JoltJobs& jobs);

} // namespace ohao::physics::backend

#endif
