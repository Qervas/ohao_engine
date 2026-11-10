#include "ecs/events.hpp"

#include <algorithm>
#include <stdexcept>

namespace ohao::ecs {

std::size_t EventChannelBase::after(uint64_t since) const noexcept {
    std::size_t lo = 0, hi = size();
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (meta(mid).seq <= since) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

const EventChannelBase* EventLog::find(std::string_view name) const {
    for (const auto& c : m_channels)
        if (c->name() == name) return c.get();
    return nullptr;
}

bool EventLog::addName(std::vector<std::string>& names, std::string_view name) {
    if (std::find(names.begin(), names.end(), name) != names.end()) return false;
    names.emplace_back(name);
    return true;
}

void EventLog::endWindow() {
    for (const auto& c : m_channels) c->m_windowBegin = c->size();
    m_windowSeq = m_nextSeq;
}

void EventLog::endStep() {
    m_inStep = false;
    endWindow();
    while (m_order.size() > kHistory) {
        const uint32_t c = m_order.front();
        m_order.pop_front();
        m_channels[c]->popFront();
    }
}

namespace {

// The next step's events: each channel's payloads, and the channels in seq order.
struct SavedWindow {
    struct Channel {
        std::string name;
        TypeKey type{0};
        std::shared_ptr<const void> values;   // a std::vector<T>
    };
    std::vector<Channel> channels;
    std::vector<uint32_t> order;   // into channels, by seq
};

} // namespace

std::shared_ptr<const void> EventLog::saveWindow() const {
    if (m_windowSeq == m_nextSeq) return nullptr;
    auto saved = std::make_shared<SavedWindow>();
    std::vector<int> slot(m_channels.size(), -1);
    walk(m_windowSeq, m_nextSeq - m_windowSeq, [&](const EventChannelBase& c, std::size_t) {
        if (slot[c.index()] < 0) {
            slot[c.index()] = static_cast<int>(saved->channels.size());
            saved->channels.push_back({c.name(), c.type(), c.saveWindow()});
        }
        saved->order.push_back(static_cast<uint32_t>(slot[c.index()]));
    });
    return saved;
}

void EventLog::restoreWindow(const std::shared_ptr<const void>& state) {
    endWindow();
    if (!state) return;
    const auto& saved = *static_cast<const SavedWindow*>(state.get());
    std::vector<EventChannelBase*> into(saved.channels.size(), nullptr);
    for (std::size_t i = 0; i < saved.channels.size(); ++i)
        if (const EventChannelBase* c = find(saved.channels[i].name); c && c->type() == saved.channels[i].type)
            into[i] = const_cast<EventChannelBase*>(c);
    std::vector<std::size_t> next(saved.channels.size(), 0);
    for (const uint32_t i : saved.order)
        if (into[i]) into[i]->emitSaved(saved.channels[i].values.get(), next[i]++);
}

} // namespace ohao::ecs
