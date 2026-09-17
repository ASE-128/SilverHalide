#pragma once

// A Qt-free bounded undo/redo stack shared by the engine and catalog layers.
// Stores full snapshots of a state type T (an Adjustments object, a string,
// etc.). push() records a new edit and discards the redo branch, so the stack
// always represents a single linear history with an optional cap.

#include <cstddef>
#include <vector>

namespace lrc {

/**
 * @brief A bounded undo/redo stack shared by the engine and catalog layers.
 *
 * Stores full snapshots of a state type T (an Adjustments object, a string,
 * etc.). push() records a new edit and discards the redo branch, so the stack
 * always represents a single linear history with an optional cap.
 *
 * @tparam T Snapshot value type (must be copyable).
 */
template <typename T>
class UndoStack {
public:
    /** @brief Constructs an empty stack.
     * @param limit Maximum number of undo states retained (default 50).
     */
    explicit UndoStack(size_t limit = 50);

    /** @brief Records a new edit.
     *
     * Clears the redo branch (starts a new linear history) and drops the
     * oldest state once the undo history exceeds the limit.
     */
    void push(const T& state);

    /** @brief Moves the most recent state into the redo stack.
     * @return False (leaving the stack untouched) when there is nothing to
     *         undo.
     */
    bool undo();

    /** @brief Inverse of undo: moves the most recently undone state back into
     *         the undo stack.
     * @return False (leaving the stack untouched) when there is nothing to
     *         redo.
     */
    bool redo();

    bool canUndo() const;
    bool canRedo() const;

    /** @brief Read-only access to the most recently pushed state.
     * @return The back of the undo stack.
     * @note Only valid while canUndo() is true (undo_ is non-empty).
     */
    const T& top() const;

    /** @brief Changes the history cap.
     *
     * Trims the oldest states immediately if the current undo history exceeds
     * the new limit.
     */
    void setLimit(size_t limit);

    /** @brief Discards all history (both undo and redo stacks). */
    void clear();

private:
    std::vector<T> undo_;  // undo history (oldest first)
    std::vector<T> redo_;  // states popped by undo() (oldest first)
    size_t limit_;
};

template <typename T>
UndoStack<T>::UndoStack(size_t limit) : limit_(limit) {}

template <typename T>
void UndoStack<T>::push(const T& state) {
    undo_.push_back(state);
    redo_.clear();
    if (undo_.size() > limit_) {
        undo_.erase(undo_.begin());
    }
}

template <typename T>
bool UndoStack<T>::undo() {
    if (undo_.empty()) {
        return false;
    }
    redo_.push_back(undo_.back());
    undo_.pop_back();
    return true;
}

template <typename T>
bool UndoStack<T>::redo() {
    if (redo_.empty()) {
        return false;
    }
    undo_.push_back(redo_.back());
    redo_.pop_back();
    return true;
}

template <typename T>
bool UndoStack<T>::canUndo() const {
    return !undo_.empty();
}

template <typename T>
bool UndoStack<T>::canRedo() const {
    return !redo_.empty();
}

template <typename T>
const T& UndoStack<T>::top() const {
    return undo_.back();
}

template <typename T>
void UndoStack<T>::setLimit(size_t limit) {
    limit_ = limit;
    while (undo_.size() > limit_) {
        undo_.erase(undo_.begin());
    }
}

template <typename T>
void UndoStack<T>::clear() {
    undo_.clear();
    redo_.clear();
}

} // namespace lrc
