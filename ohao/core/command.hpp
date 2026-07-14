#pragma once

/**
 * Undoable command stack — AI- and editor-friendly mutation history.
 *
 * Art notes:
 *   - Command is virtual (type-erased undo stack). CommandLike documents the surface.
 *   - LambdaCommand takes owned std::string + move-only callables via std::function.
 *   - execute() rejects null; undo/redo are [[nodiscard]] bool.
 */

#include "core/concepts.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ohao {

class Command {
public:
    virtual ~Command() = default;

    virtual void execute() = 0;
    virtual void undo() = 0;

    [[nodiscard]] virtual std::string getDescription() const = 0;
};

using CommandPtr = std::unique_ptr<Command>;

class LambdaCommand : public Command {
public:
    /// Takes owned description (const char* / string convert via std::string).
    LambdaCommand(std::string description,
                  std::function<void()> doFunc,
                  std::function<void()> undoFunc)
        : m_description(std::move(description))
        , m_doFunc(std::move(doFunc))
        , m_undoFunc(std::move(undoFunc)) {}

    void execute() override {
        if (m_doFunc) m_doFunc();
    }

    void undo() override {
        if (m_undoFunc) m_undoFunc();
    }

    [[nodiscard]] std::string getDescription() const override { return m_description; }

private:
    std::string m_description;
    std::function<void()> m_doFunc;
    std::function<void()> m_undoFunc;
};

static_assert(CommandLike<LambdaCommand>);

/**
 * Build a LambdaCommand from any two nullary callables (lambdas, function ptrs).
 */
template<NullaryCallable DoF, NullaryCallable UndoF>
[[nodiscard]] CommandPtr make_lambda_command(std::string_view description,
                                             DoF&& doFunc,
                                             UndoF&& undoFunc) {
    return std::make_unique<LambdaCommand>(
        std::string(description),
        std::function<void()>(std::forward<DoF>(doFunc)),
        std::function<void()>(std::forward<UndoF>(undoFunc)));
}

/**
 * CommandHistory — undo/redo stack (process-wide singleton).
 * Not thread-safe for concurrent mutate; UI/main thread only.
 */
class CommandHistory {
public:
    [[nodiscard]] static CommandHistory& instance();

    /// Execute and push. No-op if cmd is null.
    void execute(CommandPtr cmd);

    /// Factory: construct LambdaCommand and execute it.
    template<NullaryCallable DoF, NullaryCallable UndoF>
    void execute_lambda(std::string_view description, DoF&& doFunc, UndoF&& undoFunc) {
        execute(make_lambda_command(description,
                                    std::forward<DoF>(doFunc),
                                    std::forward<UndoF>(undoFunc)));
    }

    [[nodiscard]] bool undo();
    [[nodiscard]] bool redo();

    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;

    [[nodiscard]] std::string undoDescription() const;
    [[nodiscard]] std::string redoDescription() const;

    [[nodiscard]] const std::vector<CommandPtr>& getHistory() const noexcept { return m_undoStack; }
    [[nodiscard]] std::size_t getUndoCount() const noexcept { return m_undoStack.size(); }
    [[nodiscard]] std::size_t getRedoCount() const noexcept { return m_redoStack.size(); }
    [[nodiscard]] std::size_t maxHistory() const noexcept { return m_maxHistory; }

    void clear() noexcept;

    /// 0 = unlimited
    void setMaxHistory(std::size_t max) noexcept { m_maxHistory = max; }

private:
    CommandHistory() = default;

    void trimUndoStack();

    std::vector<CommandPtr> m_undoStack;
    std::vector<CommandPtr> m_redoStack;
    std::size_t m_maxHistory = 100;
};

} // namespace ohao
