#include "command.hpp"

#include <utility>

namespace ohao {

CommandHistory& CommandHistory::instance() {
    static CommandHistory history;
    return history;
}

void CommandHistory::execute(CommandPtr cmd) {
    if (!cmd) return;

    cmd->execute();
    m_undoStack.push_back(std::move(cmd));
    m_redoStack.clear();
    trimUndoStack();
}

void CommandHistory::trimUndoStack() {
    if (m_maxHistory == 0) return;
    while (m_undoStack.size() > m_maxHistory) {
        m_undoStack.erase(m_undoStack.begin());
    }
}

bool CommandHistory::undo() {
    if (m_undoStack.empty()) return false;

    auto cmd = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    cmd->undo();
    m_redoStack.push_back(std::move(cmd));
    return true;
}

bool CommandHistory::redo() {
    if (m_redoStack.empty()) return false;

    auto cmd = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    cmd->execute();
    m_undoStack.push_back(std::move(cmd));
    return true;
}

bool CommandHistory::canUndo() const noexcept {
    return !m_undoStack.empty();
}

bool CommandHistory::canRedo() const noexcept {
    return !m_redoStack.empty();
}

std::string CommandHistory::undoDescription() const {
    if (m_undoStack.empty()) return {};
    return m_undoStack.back()->getDescription();
}

std::string CommandHistory::redoDescription() const {
    if (m_redoStack.empty()) return {};
    return m_redoStack.back()->getDescription();
}

void CommandHistory::clear() noexcept {
    m_undoStack.clear();
    m_redoStack.clear();
}

} // namespace ohao
