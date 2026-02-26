#pragma once

#include <string>
#include <memory>
#include <vector>
#include <functional>

namespace ohao {

/**
 * Command - Base class for undoable operations
 *
 * Every scene mutation goes through a Command so it can be undone.
 * AI agents can experiment with changes and rollback if needed.
 */
class Command {
public:
    virtual ~Command() = default;

    // Execute the command (first time or redo)
    virtual void execute() = 0;

    // Undo the command
    virtual void undo() = 0;

    // Human-readable description for history display
    virtual std::string getDescription() const = 0;
};

using CommandPtr = std::unique_ptr<Command>;

/**
 * LambdaCommand - Quick command from two lambdas (execute + undo)
 *
 * Usage:
 *   auto cmd = std::make_unique<LambdaCommand>(
 *       "Move Box1 to (1,2,3)",
 *       [=]() { actor->setPosition(newPos); },
 *       [=]() { actor->setPosition(oldPos); }
 *   );
 *   CommandHistory::instance().execute(std::move(cmd));
 */
class LambdaCommand : public Command {
public:
    LambdaCommand(std::string description,
                  std::function<void()> doFunc,
                  std::function<void()> undoFunc)
        : m_description(std::move(description))
        , m_doFunc(std::move(doFunc))
        , m_undoFunc(std::move(undoFunc)) {}

    void execute() override { m_doFunc(); }
    void undo() override { m_undoFunc(); }
    std::string getDescription() const override { return m_description; }

private:
    std::string m_description;
    std::function<void()> m_doFunc;
    std::function<void()> m_undoFunc;
};

/**
 * CommandHistory - Undo/redo stack
 *
 * Manages a history of executed commands with undo/redo support.
 * Thread-safe singleton.
 */
class CommandHistory {
public:
    static CommandHistory& instance();

    // Execute a command and push it onto the history stack.
    // Clears any redo history.
    void execute(CommandPtr cmd);

    // Undo the last command. Returns false if nothing to undo.
    bool undo();

    // Redo the last undone command. Returns false if nothing to redo.
    bool redo();

    // Check if undo/redo is possible
    bool canUndo() const;
    bool canRedo() const;

    // Get description of what would be undone/redone
    std::string undoDescription() const;
    std::string redoDescription() const;

    // Get full history for display
    const std::vector<CommandPtr>& getHistory() const { return m_undoStack; }
    size_t getUndoCount() const { return m_undoStack.size(); }
    size_t getRedoCount() const { return m_redoStack.size(); }

    // Clear all history
    void clear();

    // Set max history size (0 = unlimited)
    void setMaxHistory(size_t max) { m_maxHistory = max; }

private:
    CommandHistory() = default;

    std::vector<CommandPtr> m_undoStack;
    std::vector<CommandPtr> m_redoStack;
    size_t m_maxHistory = 100;
};

} // namespace ohao
