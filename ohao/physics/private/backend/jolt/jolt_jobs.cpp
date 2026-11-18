// Compiled -fno-rtti (ohao/physics/CMakeLists.txt): a class derived from Jolt's needs Jolt's type info, which its
// build does not emit.
#if !defined(OHAO_HAS_JOLT) || OHAO_HAS_JOLT

#include "physics/private/backend/jolt/jolt_jobs.hpp"

#include "core/jobs/pool.hpp"
#include "core/log.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Physics/PhysicsSettings.h>

#include <chrono>
#include <thread>

namespace ohao::physics::backend {

class JoltJobs final : public JPH::JobSystemWithBarrier {
public:
    JoltJobs() : JPH::JobSystemWithBarrier(JPH::cMaxPhysicsBarriers) {
        m_jobs.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsJobs);
        m_concurrency = jobs::frameThreadCount();
        m_inline = m_concurrency == 1;
    }
    ~JoltJobs() override { finish(); }

    void setInline(bool inlineJobs) noexcept { m_inline = inlineJobs || m_concurrency == 1; }
    // Returns once every task this queued has run, so no task touches a job (or this) after the step.
    void finish() { jobs::wait(m_pending); }

    int GetMaxConcurrency() const override { return m_inline ? 1 : m_concurrency; }

    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& fn, JPH::uint32 dependencies) override {
        // Jolt asks for at most cMaxPhysicsJobs (2,048) in flight; a step at 31 threads takes a few hundred. Full,
        // a job slot comes back when a running job is freed, as in Jolt's own pool.
        JPH::uint32 index = m_jobs.ConstructObject(name, color, this, fn, dependencies);
        if (index == Jobs::cInvalidObjectIndex)
            OHAO_LOG(Warn, physics, "Jolt: all {} job slots in use; waiting for one", JPH::cMaxPhysicsJobs);
        while (index == Jobs::cInvalidObjectIndex) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            index = m_jobs.ConstructObject(name, color, this, fn, dependencies);
        }
        Job* job = &m_jobs.Get(index);
        JobHandle handle(job);   // holds a reference: the job is not freed before this returns
        if (dependencies == 0) QueueJob(job);
        return handle;
    }

    void QueueJob(Job* job) override {
        if (m_inline) return;   // the barrier's wait runs it
        job->AddRef();
        jobs::Task* task = jobs::makeTask("jolt", jobs::Priority::Frame, [job] {
            job->Execute();   // a no-op when the barrier's wait already ran it
            job->Release();
        });
        m_pending.value.fetch_add(1, std::memory_order_relaxed);
        task->counter = &m_pending;
        jobs::submit(task);
    }

    void QueueJobs(Job** queued, JPH::uint count) override {
        for (JPH::uint i = 0; i < count; ++i) QueueJob(queued[i]);
    }

    void FreeJob(Job* job) override { m_jobs.DestructObject(job); }

private:
    using Jobs = JPH::FixedSizeFreeList<Job>;
    Jobs m_jobs;
    jobs::Counter m_pending;   // the tasks queued and not yet run
    int m_concurrency{1};
    bool m_inline{false};
};

void JoltJobsDeleter::operator()(JoltJobs* jobs) const noexcept { delete jobs; }

JoltJobsPtr makeJoltJobs() { return JoltJobsPtr(new JoltJobs); }

JPH::JobSystem* joltJobSystem(JoltJobs& jobs) noexcept { return &jobs; }

void setJoltJobsInline(JoltJobs& jobs, bool inlineJobs) noexcept { jobs.setInline(inlineJobs); }

void finishJoltJobs(JoltJobs& jobs) { jobs.finish(); }

} // namespace ohao::physics::backend

#endif
