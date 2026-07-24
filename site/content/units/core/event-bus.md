---
module: core
id: event-bus
title: EventBus
---

## What

Thread-safe publish/subscribe bus for decoupling systems (physics contacts, UI, tools) without hard module links.

## How

subscribe(eventType, handler) → SubscriptionId; unsubscribe by id.

subscribeTyped<T> wraps any_cast and skips bad casts.

ScopedSubscription is move-only RAII unsubscribe.

EventBus::instance() is a process-wide convenience; inject a bus in tests.


- subscribe(eventType, handler) → SubscriptionId
- subscribeTyped<T> skips bad any_casts
- publish / publishTyped
- ScopedSubscription RAII unsubscribe

## Why

Physics and audio should not #include each other; events carry typed payloads across boundaries.

## Contracts

- string_view keys at API; owned strings in map
- ScopedSubscription moves only

## Notes

event_bus.hpp: defines `class EventBus`.

event_bus.hpp: defines `struct Subscription`.

event_bus.hpp: defines `class ScopedSubscription`.

Thread-safe publish/subscribe bus.

Art notes: - Event type keys are string_view at the API; owned std::string in the map.

- ScopedSubscription is move-only RAII unsubscribe.

- publishTyped/subscribeTyped reduce std::any boilerplate for known T.

## Notes

Source map:
- `ohao/core/event_bus.hpp`
- `ohao/core/event_bus.cpp`
