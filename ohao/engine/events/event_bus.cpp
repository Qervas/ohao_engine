#include "event_bus.hpp"

namespace ohao {

EventBus& EventBus::instance() {
    static EventBus bus;
    return bus;
}

SubscriptionId EventBus::subscribe(const std::string& eventType, EventHandler handler) {
    std::lock_guard<std::mutex> lock(m_mutex);
    SubscriptionId id = m_nextId++;
    m_subscribers[eventType].push_back({id, std::move(handler)});
    return id;
}

void EventBus::unsubscribe(SubscriptionId id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [type, subs] : m_subscribers) {
        subs.erase(
            std::remove_if(subs.begin(), subs.end(),
                [id](const Subscription& s) { return s.id == id; }),
            subs.end());
    }
}

void EventBus::publish(const std::string& eventType, std::any data) {
    std::vector<EventHandler> handlers;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_subscribers.find(eventType);
        if (it == m_subscribers.end()) return;
        // Copy handlers to call outside the lock
        for (const auto& sub : it->second) {
            handlers.push_back(sub.handler);
        }
    }

    Event event{eventType, std::move(data)};
    for (const auto& handler : handlers) {
        handler(event);
    }
}

void EventBus::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_subscribers.clear();
}

} // namespace ohao
