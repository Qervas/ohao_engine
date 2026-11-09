#pragma once
// The World's events (step 3c; docs/plans/2026-11-09-step3-engine-game-boundary.md, "3c").
//
// A channel is a name and a payload type: `world.events().writer<FootEvent>("physics.footstep", "physics")`. A
// system holds the writers and readers it takes at install; each names the system, so the scheduler (step 3h)
// learns what a system emits and reads from what it holds. A name taken with a second payload type throws.
//
//   - What a step reads: a reader's read() is its channel's events for this step, oldest first. An event emitted
//     between steps (an API call, a body removed in a sync) belongs to the next step. A channel's writers run
//     before its readers (ecs/system_order.hpp), so a step's events are read in that step and a reader keeps no
//     state.
//   - State: the events the next step will read. A snapshot taken between steps carries them (saveWindow), and
//     world.hash covers them; a restore drops the next step's events and puts the snapshot's back. A snapshot taken
//     during a step carries none, and a restore during a step (a round's restart) ends the step's events: what the
//     step had emitted was undone with the world, so the systems after it read only what follows.
//   - History: every event also stays in one history of the last kHistory, by seq (from 1, no gaps across
//     channels). History is never rewound, hashed or snapshotted: a restore's events get new seqs. Observers (the
//     API's events.read, the HUD, the audio player) read it by cursor. Events leave it only at a step's end, never
//     one a step has yet to read.
//   - An event's identity is its step, its channel and its place among that step's events of the channel: a
//     restore and a replay keep it, unlike its seq.
//
// Payloads are plain structs, reflected (TypeRole::Event) where the API writes them and world.hash hashes them;
// they hold entities by EntityId, since an event outlives what it names. No reflection here: an emit is two
// appends, a read a span.
//
// The engine/game boundary: the engine reads its own channels by type, and a game's only as a Cue, which the
// game's projection (EventChannel::setCue) makes from its payload; the engine's data binds cues by channel name
// (an audio bank's and a vfx set's `events`). No engine system includes a game's payload type.
#include "core/type_key.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ohao::ecs {

struct EventMeta {
    uint64_t seq{0};    // its place in the history (history: never rewound or hashed)
    uint64_t step{0};   // the World step that reads it
};

template <class T>
struct Event {
    EventMeta meta;
    T value;
};

// An event as the engine's data binds it: what its channel's projection makes of its payload. Fields the
// projection leaves are zero (the tag empty).
struct Cue {
    glm::vec3 at{0.0f};               // where it happened
    std::optional<glm::vec3> to;      // where it ended (a round's stop), when it did
    glm::vec3 dir{0.0f};              // its motion's direction (a round's), or zero
    glm::vec3 normal{0.0f};           // the surface's normal, or zero
    uint64_t source{0}, target{0};    // entity ids: who caused it, what it reached
    std::string_view tag;             // a name a binding matches or puts in for {tag} (a surface, "empty")
    uint32_t n{0};                    // a count its emitter keeps (a weapon's rounds)
};

class EventLog;

// A channel's untyped side: the log, the API (which writes payloads through reflection) and world.hash.
class EventChannelBase {
public:
    virtual ~EventChannelBase() = default;
    EventChannelBase(const EventChannelBase&) = delete;
    EventChannelBase& operator=(const EventChannelBase&) = delete;

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] TypeKey type() const noexcept { return m_type; }   // the payload's (reflect::TypeRegistry::find)
    [[nodiscard]] uint32_t index() const noexcept { return m_index; }   // in the log's channels()
    [[nodiscard]] const std::vector<std::string>& writers() const noexcept { return m_writers; }   // systems
    [[nodiscard]] const std::vector<std::string>& readers() const noexcept { return m_readers; }

    // The events held, oldest first: the history's and the next (or running) step's.
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual const EventMeta& meta(std::size_t i) const noexcept = 0;
    [[nodiscard]] virtual const void* payload(std::size_t i) const noexcept = 0;
    // Where the step's events start among those held.
    [[nodiscard]] std::size_t windowBegin() const noexcept { return m_windowBegin; }
    // The first held event with seq > since (size() when none).
    [[nodiscard]] std::size_t after(uint64_t since) const noexcept;
    // The event's cue (false: the channel has no projection).
    [[nodiscard]] virtual bool cue(std::size_t i, Cue& out) const = 0;

protected:
    EventChannelBase(EventLog& log, std::string name, TypeKey type, uint32_t index)
        : m_log(log), m_name(std::move(name)), m_type(type), m_index(index) {}
    EventLog& m_log;
    std::size_t m_windowBegin{0};

private:
    friend class EventLog;
    virtual void popFront() = 0;
    [[nodiscard]] virtual std::shared_ptr<const void> saveWindow() const = 0;   // the step's payloads, a vector<T>
    virtual void emitSaved(const void* saved, std::size_t i) = 0;

    std::string m_name;
    TypeKey m_type;
    uint32_t m_index;
    std::vector<std::string> m_writers, m_readers;
};

template <class T>
class EventChannel final : public EventChannelBase {
public:
    EventChannel(EventLog& log, std::string name, uint32_t index)
        : EventChannelBase(log, std::move(name), typeKey<T>(), index) {}

    uint64_t emit(T value);   // its seq
    // The projection the engine's data binds to (a game's channels; engine channels need none).
    using CueFn = Cue (*)(const T&);
    void setCue(CueFn fn) noexcept { m_cue = fn; }
    [[nodiscard]] bool cue(std::size_t i, Cue& out) const override {
        if (!m_cue) return false;
        out = m_cue(at(i).value);
        return true;
    }

    // This step's events (between steps: the next step's so far).
    [[nodiscard]] std::span<const Event<T>> window() const noexcept {
        return {m_held.data() + m_front + m_windowBegin, m_held.size() - m_front - m_windowBegin};
    }
    // Held events with seq > since, oldest first.
    [[nodiscard]] std::span<const Event<T>> since(uint64_t seq) const noexcept {
        const std::size_t i = after(seq);
        return {m_held.data() + m_front + i, m_held.size() - m_front - i};
    }
    [[nodiscard]] const Event<T>& at(std::size_t i) const noexcept { return m_held[m_front + i]; }

    [[nodiscard]] std::size_t size() const noexcept override { return m_held.size() - m_front; }
    [[nodiscard]] const EventMeta& meta(std::size_t i) const noexcept override { return m_held[m_front + i].meta; }
    [[nodiscard]] const void* payload(std::size_t i) const noexcept override { return &m_held[m_front + i].value; }

private:
    void popFront() override {
        ++m_front;
        if (m_windowBegin > 0) --m_windowBegin;
        if (m_front >= 64 && 2 * m_front >= m_held.size()) {   // amortized: the vector stays contiguous
            m_held.erase(m_held.begin(), m_held.begin() + static_cast<std::ptrdiff_t>(m_front));
            m_front = 0;
        }
    }
    [[nodiscard]] std::shared_ptr<const void> saveWindow() const override {
        auto out = std::make_shared<std::vector<T>>();
        for (const Event<T>& e : window()) out->push_back(e.value);
        return out;
    }
    void emitSaved(const void* saved, std::size_t i) override {
        emit(static_cast<const std::vector<T>*>(saved)->at(i));
    }

    std::vector<Event<T>> m_held;
    std::size_t m_front{0};
    CueFn m_cue{nullptr};
};

// What a system emits into. Empty (false) when default-constructed.
template <class T>
class EventWriter {
public:
    EventWriter() = default;
    explicit EventWriter(EventChannel<T>* c) noexcept : m_c(c) {}
    uint64_t emit(T value) const { return m_c->emit(std::move(value)); }
    [[nodiscard]] EventChannel<T>* channel() const noexcept { return m_c; }
    explicit operator bool() const noexcept { return m_c != nullptr; }

private:
    EventChannel<T>* m_c{nullptr};
};

// What a system reads: its channel's events for this step.
template <class T>
class EventReader {
public:
    EventReader() = default;
    explicit EventReader(const EventChannel<T>* c) noexcept : m_c(c) {}
    [[nodiscard]] std::span<const Event<T>> read() const noexcept {
        return m_c ? m_c->window() : std::span<const Event<T>>{};
    }
    [[nodiscard]] const EventChannel<T>* channel() const noexcept { return m_c; }

private:
    const EventChannel<T>* m_c{nullptr};
};

class EventLog {
public:
    static constexpr std::size_t kHistory = 8192;   // events the history keeps

    EventLog() = default;
    EventLog(const EventLog&) = delete;
    EventLog& operator=(const EventLog&) = delete;

    // The channel `name` (made on first use). Throws std::logic_error when it carries another payload type.
    template <class T>
    EventChannel<T>& channel(std::string_view name);
    template <class T>
    EventWriter<T> writer(std::string_view name, std::string_view system) {
        EventChannel<T>& c = channel<T>(name);
        addName(c.m_writers, system);
        return EventWriter<T>(&c);
    }
    template <class T>
    EventReader<T> reader(std::string_view name, std::string_view system) {
        EventChannel<T>& c = channel<T>(name);
        addName(c.m_readers, system);
        return EventReader<T>(&c);
    }
    // A system that reads a channel through its cues (an engine system bound by data): named among its readers.
    // False when no such channel exists yet.
    bool addReader(std::string_view name, std::string_view system) {
        const EventChannelBase* c = find(name);
        if (c) addName(const_cast<EventChannelBase*>(c)->m_readers, system);
        return c != nullptr;
    }
    // The channel, or null (none of that name, or another payload type).
    template <class T>
    [[nodiscard]] const EventChannel<T>* find(std::string_view name) const {
        const EventChannelBase* c = find(name);
        return c && c->type() == typeKey<T>() ? static_cast<const EventChannel<T>*>(c) : nullptr;
    }
    [[nodiscard]] const EventChannelBase* find(std::string_view name) const;
    [[nodiscard]] std::span<const std::unique_ptr<EventChannelBase>> channels() const noexcept { return m_channels; }

    [[nodiscard]] uint64_t nextSeq() const noexcept { return m_nextSeq; }   // the seq the next event gets
    [[nodiscard]] uint64_t oldestSeq() const noexcept { return m_nextSeq - m_order.size(); }   // nextSeq when empty
    // Every held event with seq > since, in seq order, at most max: f(const EventChannelBase&, std::size_t i), which
    // may return false to stop before that event. Returns the cursor to pass next: the last seq visited, else since
    // (never past the history). f must not emit.
    template <class F>
    uint64_t visit(uint64_t since, std::size_t max, F&& f) const;
    // The next (or running) step's events, in seq order: f(const EventChannelBase&, std::size_t i).
    template <class F>
    void visitWindow(F&& f) const;

    // The World's step (World::steps()): what an event is stamped with, the next one's between steps.
    [[nodiscard]] uint64_t step() const noexcept { return m_step; }
    [[nodiscard]] bool inStep() const noexcept { return m_inStep; }
    void beginStep(uint64_t step) noexcept {
        m_step = step;
        m_inStep = true;
    }
    // The step's events become history; the history drops what is over kHistory.
    void endStep();
    void setStep(uint64_t step) noexcept { m_step = step; }   // a restore
    // A restore: the step's (or the next step's) events are history, then the snapshot's come back.
    [[nodiscard]] std::shared_ptr<const void> saveWindow() const;   // null during a step, or with none
    void restoreWindow(const std::shared_ptr<const void>& saved);

private:
    template <class T>
    friend class EventChannel;
    static void addName(std::vector<std::string>& names, std::string_view name);
    void endWindow();
    EventMeta stamp(uint32_t channel) {
        m_order.push_back(channel);
        return EventMeta{m_nextSeq++, m_inStep ? m_step : m_step + 1};
    }
    template <class F>
    void walk(uint64_t from, std::size_t max, F&& f) const;   // seq >= from, in order

    std::vector<std::unique_ptr<EventChannelBase>> m_channels;
    std::deque<uint32_t> m_order;   // the channel of every held event, by seq (the oldest first)
    uint64_t m_nextSeq{1};
    uint64_t m_windowSeq{1};        // the step's first event
    uint64_t m_step{0};
    bool m_inStep{false};
};

template <class T>
uint64_t EventChannel<T>::emit(T value) {
    const EventMeta meta = m_log.stamp(index());
    m_held.push_back(Event<T>{meta, std::move(value)});
    return meta.seq;
}

template <class T>
EventChannel<T>& EventLog::channel(std::string_view name) {
    if (const EventChannelBase* c = find(name)) {
        if (c->type() != typeKey<T>())
            throw std::logic_error("event channel '" + std::string(name) + "' already carries another payload type");
        return *static_cast<EventChannel<T>*>(const_cast<EventChannelBase*>(c));
    }
    auto c = std::make_unique<EventChannel<T>>(*this, std::string(name), static_cast<uint32_t>(m_channels.size()));
    EventChannel<T>& ref = *c;
    m_channels.push_back(std::move(c));
    return ref;
}

template <class F>
void EventLog::walk(uint64_t from, std::size_t max, F&& f) const {
    const uint64_t oldest = oldestSeq();
    if (from < oldest) from = oldest;
    if (from >= m_nextSeq || max == 0) return;
    // Each channel's first held event at or after `from`, then the channels in seq order.
    std::vector<std::size_t> pos(m_channels.size(), 0);
    for (std::size_t c = 0; c < m_channels.size(); ++c) pos[c] = m_channels[c]->after(from - 1);
    std::size_t n = 0;
    for (std::size_t k = static_cast<std::size_t>(from - oldest); k < m_order.size() && n < max; ++k, ++n) {
        const uint32_t c = m_order[k];
        if constexpr (std::is_same_v<std::invoke_result_t<F&, const EventChannelBase&, std::size_t>, bool>) {
            if (!f(*m_channels[c], pos[c]++)) return;
        } else {
            f(*m_channels[c], pos[c]++);
        }
    }
}

template <class F>
uint64_t EventLog::visit(uint64_t since, std::size_t max, F&& f) const {
    uint64_t next = since < m_nextSeq ? since : m_nextSeq - 1;
    walk(since + 1, max, [&](const EventChannelBase& c, std::size_t i) {
        if constexpr (std::is_same_v<std::invoke_result_t<F&, const EventChannelBase&, std::size_t>, bool>) {
            if (!f(c, i)) return false;
        } else {
            f(c, i);
        }
        next = c.meta(i).seq;
        return true;
    });
    return next;
}

template <class F>
void EventLog::visitWindow(F&& f) const {
    walk(m_windowSeq, m_nextSeq - m_windowSeq, f);
}

} // namespace ohao::ecs
