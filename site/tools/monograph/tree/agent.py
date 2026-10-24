"""Tree units: 13 · Agent control surface (sub-project 4)."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'agent',
    "title": '13 · Agent control surface',
    "hub": None,
    "children": [
        page(
            'protocol',
            'The protocol server',
            'JSON-RPC 2.0 and MCP on one server: a reader thread and an engine thread, cancellation, progress, the error table, tools from params structs, a bounded queue.',
            files=['ohao/rpc/server.hpp', 'ohao/rpc/server.cpp'],
        ),
        page(
            'transports',
            'Transports: stdio and the named pipe',
            'Newline framing without holding oversize lines, stdout kept for the protocol, interruptible reads, and a pipe only the current user on this machine can open.',
            files=['ohao/rpc/transport.hpp', 'ohao/rpc/transport.cpp', 'tools/ohao/main.cpp'],
        ),
        page(
            'live-host',
            'The live host',
            'One window loop for ohao_game and ohao serve --window: requests at a fixed point of the frame, render on change, replies after present.',
            files=['ohao/runtime/live_host.hpp', 'ohao/runtime/live_host.cpp', 'ohao/runtime/window.hpp',
                   'ohao/runtime/window.cpp', 'examples/ohao_game.cpp'],
        ),
        page(
            'time-input',
            'Time and input as data',
            'Lockstep and realtime on one step axis, an input map stepped per simulation step, injection, and recordings that check themselves.',
            files=['ohao/core/timeline.hpp', 'ohao/core/timeline.cpp', 'ohao/core/input.hpp', 'ohao/core/input.cpp',
                   'ohao/api/input.cpp', 'ohao/ecs/fly_controller_system.cpp'],
        ),
        page(
            'capture',
            'Frame capture',
            'One capture procedure for both hosts, readback on request, previews beside the result, and a fast lossless PNG writer.',
            files=['ohao/runtime/frame_capture.hpp', 'ohao/runtime/frame_capture.cpp', 'ohao/runtime/image.hpp',
                   'ohao/runtime/image.cpp', 'ohao/gpu/vulkan/render_dispatch.cpp', 'tools/ohao/render_host.cpp'],
        ),
        page(
            'hash-diff',
            'World hash, diff and determinism',
            'A 64-bit fingerprint of the authoring state, changed fields against a snapshot, and bit-identical lockstep worlds and deferred captures.',
            files=['ohao/ecs/world_hash.hpp', 'ohao/ecs/world_hash.cpp', 'ohao/ecs/world_diff.hpp',
                   'ohao/ecs/world_diff.cpp', 'ohao/reflect/hash.hpp'],
        ),
    ],
}
