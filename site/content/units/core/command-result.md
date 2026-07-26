---
module: core
id: command-result
title: Command history & Result
standard: v2
---

## Undo as replay, not as snapshot

There are two ways to build an undo stack. One stores state — copy the scene
before the edit, restore the copy on undo. The other stores *operations* — a
pair of closures, one that applies a change and one that reverses it — and never
copies anything. OHAO takes the second, which is why `Command` is three pure
virtuals and no data.

{{cite ohao/core/command.hpp "virtual void execute() = 0;"}}

The consequence people miss is in `redo()`. It does not replay a recorded delta;
it pops the command back off the redo stack and calls `execute()` on the *same
object* a second time.

{{cite ohao/core/command.cpp "auto cmd = std::move(m_redoStack.back());"}}

So a command's do-function is not a one-shot. It must be able to run again from
the state its own undo-function left behind, any number of times. A closure that
captures "the entity I just created" by pointer, or that computes a delta from
whatever the world happened to look like at construction, will execute correctly
once and corrupt the scene on the first redo. That is the contract, and nothing
in the type system enforces it.

The complementary rule lives in the push path: `CommandHistory::execute` clears
the redo stack after every accepted command, so the model is a strictly linear
timeline — undo three, do something new, and the three futures are gone. No redo
tree, no branching history.

{{cite ohao/core/command.cpp "void CommandHistory::execute(CommandPtr cmd) {"}}

## What the stack caps, and what the cap costs

History is bounded at 100 entries by default, with 0 meaning unlimited.

{{cite ohao/core/command.hpp "std::size_t m_maxHistory = 100;"}}

Trimming is the one place the data structure choice shows. The undo stack is a
`std::vector`, so dropping the oldest entry means erasing at the front and
shifting everything down.

{{cite ohao/core/command.cpp "m_undoStack.erase(m_undoStack.begin());"}}

A `std::deque` would make that O(1). It is a real inefficiency and an irrelevant
one: `trimUndoStack` is called from exactly one site, the tail of
`CommandHistory::execute`, so in steady state the loop erases exactly one element
and shifts 100 `unique_ptr`s, once per user-initiated editor action.

{{cite ohao/core/command.cpp "    trimUndoStack();"}}

`redo()` is the other place the undo stack grows, and it does not trim. That is
safe only because `execute` clears the redo stack: everything `redo()` can push
came off the undo stack in the first place, so it was already counted under the
cap.

{{cite ohao/core/command.cpp "bool CommandHistory::redo() {"}}

`instance()` returns a function-local static, so construction is thread-safe by
the C++11 magic-statics rule while every mutation on it is not — the header
restricts the object to the UI/main thread and no lock exists to back that up.

## A concept that documents, and a concept that constrains

`concepts.hpp` declares `CommandLike` — a requires-clause spelling out
`execute`, `undo`, `getDescription` structurally, with no inheritance involved.

{{cite ohao/core/concepts.hpp "concept CommandLike = requires(T& t, const T& ct) {"}}

It constrains nothing. It cannot: the undo stack is heterogeneous at runtime, so
it must hold `unique_ptr<Command>` and dispatch through a vtable, and a concept
cannot type-erase. What the concept buys is a compile-time assertion that the
concrete command type and the documented surface have not drifted apart.

{{cite ohao/core/command.hpp "static_assert(CommandLike<LambdaCommand>);"}}

`NullaryCallable` — `std::invocable` plus a `void` result — is the one that does
real work, gating `make_lambda_command` and `execute_lambda` so a lambda with the
wrong arity produces a constraint failure at the call site instead of a page of
`std::function` instantiation errors.

## execute() has no way to say no

`Command::execute()` returns `void`. A command that tries to load a mesh and
fails has no channel to report it; `CommandHistory::execute` pushes it onto the
undo stack regardless, and a later `undo()` will faithfully reverse a mutation
that never happened. `undo()` and `redo()` are `[[nodiscard]] bool`, but that
bool only reports "the stack was empty" — never "the command failed".

This is the seam between the two halves of this unit. The engine has a type for
exactly this — a fallible return that is not an exception — and the command
interface predates using it.

## The half of std::expected that got written

`Result<T, E = std::string>` is a hand-rolled stand-in for C++23's
`std::expected`, storing the two outcomes in a `std::variant<T, E>`. It is a
strict subset, and the missing part is the interesting one: there is no
`and_then`, `or_else`, `transform` or `transform_error`, no `unexpected<E>`
wrapper to return, no `error_or`, no `bad_expected_access`. Nothing downstream
can chain on a `Result`; a caller can only branch on it, immediately, at the call
site.

What did get written is careful about holes. The default constructor is private
and seeds the *error* alternative with a value-initialised `E`, so there is no
third "empty" state to test for — the only public ways in are the named `ok` and
`err` factories, both `[[nodiscard]]`.

{{cite ohao/core/result.hpp "Result() : m_storage(std::in_place_type<E>, E{}) {}"}}

The intended call shape is a condition-declaration: it binds the result, tests it
contextually, and keeps both `value()` and `error()` in scope for their
respective branches.

{{cite examples/model_viewer.cpp "if (auto loadedModel = ModelLoader::loadResult(modelPath)) {"}}

`operator bool` being explicit is not what permits that — a condition converts
contextually either way. What `explicit` buys is the rejection of everything
else: `int n = r`, `r == 1` and `r + 1` are all ill-formed, so a `Result` cannot
leak into arithmetic or a comparison by accident.

:::why
The alternative is throwing. The tree shows where that line was drawn: there are
seven `throw std::runtime_error` sites in `ohao/`, and every one of them is a
broken build or device invariant — a compiled shader missing from disk, a memory
type the physical device does not expose. A missing model file is not a bug, so
`ModelLoader::loadResult` validates the path and the extension before it even
tries and returns the reason as a string. The cost of that choice is that a
`Result` is only as good as the caller's willingness to test it, where an
exception would have been impossible to ignore.
:::

## The Result you can name but cannot use

`Result<std::string>` instantiates cleanly — the class template has a size and
the declaration compiles, because member bodies are only instantiated on use.
Every use is then ill-formed. With the default error type the storage is
`std::variant<std::string, std::string>`, and `variant::emplace<T>` is defined as
deleted when `T` does not appear exactly once in the alternatives, so both
factories fail at the point of call.

{{cite ohao/core/result.hpp "r.m_storage.template emplace<T>(std::move(value));"}}

`has_value()` and `operator bool` break the same way on `holds_alternative<T>`;
`value()`, `error()` and `value_or` break on `std::get<T>`; and the private
default constructor cannot even name its own alternative, because
`std::in_place_type<E>` is ambiguous when `E` is also `T`.

{{cite ohao/core/result.hpp "return std::holds_alternative<T>(m_storage);"}}

None of that shows at the declaration, which is what makes it a hazard for the
obvious next use — a loader that returns file text, a shader preprocessor
returning source. The fix is to pass an explicit error type that differs from
`T`, not to reach for `value_or`.

Two smaller sharp edges sit next to it. `value()` forwards to `std::get`, so
calling it on an error `Result` throws `std::bad_variant_access` — the type
removes exceptions from expected failure, not from misuse. `VoidResult` gets that
wrong in the opposite direction: it stores its error in a plain member and hands
it back unconditionally, so `error()` on a successful `VoidResult` silently
returns an empty string instead of complaining.

{{cite ohao/core/result.hpp "[[nodiscard]] const E& error() const& { return m_error; }"}}

## What actually uses any of this

Both halves are compiled, exported through the `core.hpp` umbrella, and covered
by `tests/engine/engine_tests.cpp`. Neither is load-bearing.

`CommandHistory` has zero callers outside that test file — no scene edit, no
material tweak, no transform gizmo routes through it, and no other undo stack is
hiding elsewhere in the tree under a different name. `Result` has exactly one
production producer, `ModelLoader::loadResult`, consumed by two examples. The
`VoidResult`-returning Vulkan wrapper has no callers at all, despite the GPU
module's own header advertising it as house style.

{{cite ohao/gpu/vulkan/vk_utils.hpp "[[nodiscard]] inline VoidResult<> vk_check(VkResult r, std::string_view context) {"}}

:::key
Read these two headers as a declared intent rather than as a description of the
running engine. They define the shape an editor and a fallible API are supposed
to take; the renderer still reports failure with `bool` returns, null
`shared_ptr`s, and seven throws. The gap between the vocabulary and its adoption
is the honest state of `core/`.
:::

## Contracts

- A command's do-function must be replayable, not idempotent: `redo()` calls `execute()` on the same object again, from the state `undo()` produced. Running it twice with no intervening undo is not required to be safe. Capturing construction-time context that the undo already invalidated works once and breaks on redo.
- `CommandHistory::execute` clears the redo stack. Undo history is linear; there is no branch to return to.
- Mutating the history from anything but the main thread is undefined — `instance()` is thread-safe to *initialise* only.
- `Result<T, E>` requires `T` and `E` to be distinct types. `Result<std::string>` names an instantiable type on which every operation — both factories, `has_value`, `value`, `error`, `value_or` — is ill-formed.
- `value()` on an error `Result` throws; test with `operator bool` first. `VoidResult::error()` on success does not throw and returns a default-constructed error instead.
