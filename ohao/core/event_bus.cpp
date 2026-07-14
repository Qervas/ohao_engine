#include "event_bus.hpp"

#include <utility>

namespace ohao {

EventBus& EventBus::instance() {
    static EventBus bus;
    return bus;
}

SubscriptionId EventBus::subscribe(std::string_view eventType, EventHandler handler) {
    std::lock_guard lock(m_mutex);
    const SubscriptionId id = m_nextId++;
    m_subscribers[std::string(eventType)].push_back(Subscription{
        .id = id,
        .handler = std::move(handler),
    });
    return id;
}

void EventBus::unsubscribe(SubscriptionId id) {
    if (id == kInvalidSubscriptionId) return;

    std::lock_guard lock(m_mutex);
    for (auto& [type, subs] : m_subscribers) {
        std::erase_if(subs, [id](const Subscription& s) { return s.id == id; });
    }
}

void EventBus::publish(std::string_view eventType, std::any data) {
    std::vector<EventHandler> handlers;
    const std::string key(eventType);

    {
        std::lock_guard lock(m_mutex);
        const auto it = m_subscribers.find(key);
        if (it == m_subscribers.end()) return;
        handlers.reserve(it->second.size());
        for (const auto& sub : it->second) {
            handlers.push_back(sub.handler);
        }
    }

    const Event event{
        .type = key,
        .data = std::move(data),
    };
    for (const auto& handler : handlers) {
        if (handler) handler(event);
    }
}

void EventBus::clear() {
    std::lock_guard lock(m_mutex);
    m_subscribers.clear();
}

std::size_t EventBus::subscriptionCount() const {
    std::lock_guard lock(m_mutex);
    std::size_t n = 0;
    for (const auto& [type, subs] : m_subscribers) {
        n += subs.size();
    }
    return n;
}

} // namespace ohao
