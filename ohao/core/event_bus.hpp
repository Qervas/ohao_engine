#pragma once

/**
 * Thread-safe publish/subscribe bus.
 *
 * Art notes:
 *   - Event type keys are string_view at the API; owned std::string in the map.
 *   - ScopedSubscription is move-only RAII unsubscribe.
 *   - publishTyped/subscribeTyped reduce std::any boilerplate for known T.
 */

#include <any>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ohao {

struct Event {
    std::string type;
    std::any data;

    template<typename T>
    [[nodiscard]] bool holds() const noexcept {
        return data.has_value() && data.type() == typeid(T);
    }

    template<typename T>
    [[nodiscard]] const T* try_cast() const noexcept {
        return std::any_cast<T>(&data);
    }
};

using EventHandler = std::function<void(const Event&)>;
using SubscriptionId = std::uint64_t;

inline constexpr SubscriptionId kInvalidSubscriptionId = 0;

class EventBus {
public:
    EventBus() = default;

    /// Process-wide bus (optional convenience). Prefer an injected EventBus when testing.
    [[nodiscard]] static EventBus& instance();

    [[nodiscard]] SubscriptionId subscribe(std::string_view eventType, EventHandler handler);

    /// Typed subscribe: handler receives T (copied from any). Silently skips bad casts.
    template<typename T, typename F>
        requires std::invocable<F&, const T&>
    [[nodiscard]] SubscriptionId subscribeTyped(std::string_view eventType, F&& handler) {
        return subscribe(eventType, [h = std::function<void(const T&)>(std::forward<F>(handler))](const Event& e) {
            if (const T* p = e.try_cast<T>()) {
                h(*p);
            }
        });
    }

    void unsubscribe(SubscriptionId id);

    void publish(std::string_view eventType, std::any data = {});

    template<typename T>
    void publishTyped(std::string_view eventType, T&& value) {
        publish(eventType, std::any(std::forward<T>(value)));
    }

    void clear();

    /// Approximate subscriber count for diagnostics (locked).
    [[nodiscard]] std::size_t subscriptionCount() const;

private:
    struct Subscription {
        SubscriptionId id{kInvalidSubscriptionId};
        EventHandler handler;
    };

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::vector<Subscription>> m_subscribers;
    SubscriptionId m_nextId = 1;
};

/**
 * RAII unsubscribe. Move-only. Default-constructed is empty.
 */
class ScopedSubscription {
public:
    ScopedSubscription() = default;

    ScopedSubscription(EventBus& bus, SubscriptionId id) noexcept
        : m_bus(&bus), m_id(id) {}

    ~ScopedSubscription() { reset(); }

    ScopedSubscription(const ScopedSubscription&) = delete;
    ScopedSubscription& operator=(const ScopedSubscription&) = delete;

    ScopedSubscription(ScopedSubscription&& other) noexcept
        : m_bus(std::exchange(other.m_bus, nullptr))
        , m_id(std::exchange(other.m_id, kInvalidSubscriptionId)) {}

    ScopedSubscription& operator=(ScopedSubscription&& other) noexcept {
        if (this != &other) {
            reset();
            m_bus = std::exchange(other.m_bus, nullptr);
            m_id = std::exchange(other.m_id, kInvalidSubscriptionId);
        }
        return *this;
    }

    void reset() noexcept {
        if (m_bus && m_id != kInvalidSubscriptionId) {
            m_bus->unsubscribe(m_id);
        }
        m_bus = nullptr;
        m_id = kInvalidSubscriptionId;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return m_bus != nullptr && m_id != kInvalidSubscriptionId;
    }

    [[nodiscard]] SubscriptionId id() const noexcept { return m_id; }

private:
    EventBus* m_bus{nullptr};
    SubscriptionId m_id{kInvalidSubscriptionId};
};

/// Subscribe and wrap in RAII guard.
[[nodiscard]] inline ScopedSubscription make_scoped_subscription(EventBus& bus,
                                                                 std::string_view eventType,
                                                                 EventHandler handler) {
    return ScopedSubscription{bus, bus.subscribe(eventType, std::move(handler))};
}

} // namespace ohao
