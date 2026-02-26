#pragma once

#include <functional>
#include <unordered_map>
#include <vector>
#include <string>
#include <any>
#include <mutex>
#include <cstdint>

namespace ohao {

/**
 * EventBus - Decoupled publish/subscribe event system
 *
 * Allows engine components to communicate without direct references.
 * Thread-safe. Supports typed event data via std::any.
 *
 * Usage:
 *   EventBus& bus = EventBus::instance();
 *   auto id = bus.subscribe("actor.selected", [](const Event& e) {
 *       auto name = std::any_cast<std::string>(e.data);
 *   });
 *   bus.publish("actor.selected", std::string("MyActor"));
 *   bus.unsubscribe(id);
 */

struct Event {
    std::string type;
    std::any data;
};

using EventHandler = std::function<void(const Event&)>;
using SubscriptionId = uint64_t;

class EventBus {
public:
    static EventBus& instance();

    // Subscribe to an event type. Returns a subscription ID for unsubscribing.
    SubscriptionId subscribe(const std::string& eventType, EventHandler handler);

    // Unsubscribe by ID
    void unsubscribe(SubscriptionId id);

    // Publish an event to all subscribers
    void publish(const std::string& eventType, std::any data = {});

    // Clear all subscriptions
    void clear();

private:
    EventBus() = default;

    struct Subscription {
        SubscriptionId id;
        EventHandler handler;
    };

    std::mutex m_mutex;
    std::unordered_map<std::string, std::vector<Subscription>> m_subscribers;
    SubscriptionId m_nextId = 1;
};

} // namespace ohao
