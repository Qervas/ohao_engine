"""Tree units: 01b · Core."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'core',
    "title": '01b · Core',
    "hub": None,
    "children": [
        page(
            'types-concepts',
            'Types & concepts',
            'GpuPod, common_types, C++20 concepts used across modules.',
            files=['ohao/core/common_types.hpp', 'ohao/core/concepts.hpp', 'ohao/core/core.hpp'],
            design=['GpuPod constrains GPU-shared structs to trivially copyable layouts.', 'common_types holds shared enums/aliases used by scene and renderer.'],
        ),
        page(
            'event-bus',
            'EventBus',
            'Thread-safe pub/sub with typed subscribe helpers.',
            files=['ohao/core/event_bus.hpp', 'ohao/core/event_bus.cpp'],
            workflow=['subscribe(eventType, handler) → SubscriptionId', 'subscribeTyped<T> skips bad any_casts', 'publish / publishTyped', 'ScopedSubscription RAII unsubscribe'],
            why='Decouple systems (physics contacts, UI) without hard links.',
        ),
        page(
            'command-result',
            'Command history & Result',
            'Undo/redo command pattern and fallible Result type.',
            files=['ohao/core/command.hpp', 'ohao/core/command.cpp', 'ohao/core/result.hpp'],
            design=['Result avoids exceptions on expected failures in loaders and init paths.'],
        ),
        page(
            'log',
            'Logger',
            'OHAO_LOG: levels, channels, a 4,096-record ring and sinks; every engine message.',
            files=['ohao/core/log.hpp', 'ohao/core/log.cpp'],
            design=['Below the threshold a call is one atomic load and a compare; arguments are not evaluated.', 'The ring is always on, so an agent can page recent records; stdout is never written.'],
        ),
    ],
}
