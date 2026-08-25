#include <catch2/catch_test_macros.hpp>

#include <string>

#include "utils/UndoStack.h"

TEST_CASE("UndoStack starts empty", "[undo]") {
    lrc::UndoStack<std::string> stack;
    REQUIRE_FALSE(stack.canUndo());
    REQUIRE_FALSE(stack.canRedo());
    REQUIRE_FALSE(stack.undo());
    REQUIRE_FALSE(stack.redo());
}

TEST_CASE("UndoStack push then undo then redo restores the state", "[undo]") {
    lrc::UndoStack<std::string> stack;
    stack.push("v1");
    REQUIRE(stack.canUndo());
    REQUIRE_FALSE(stack.canRedo());

    // Undo moves "v1" out; there is nothing left to undo.
    REQUIRE(stack.undo());
    REQUIRE_FALSE(stack.canUndo());
    REQUIRE(stack.canRedo());

    // Redo brings it back.
    REQUIRE(stack.redo());
    REQUIRE(stack.canUndo());
    REQUIRE_FALSE(stack.canRedo());
    REQUIRE_FALSE(stack.redo());
}

TEST_CASE("UndoStack multiple pushes undo back to the oldest then redo forward",
          "[undo]") {
    lrc::UndoStack<int> stack;
    stack.push(1);
    stack.push(2);
    stack.push(3);
    REQUIRE(stack.canUndo());
    REQUIRE_FALSE(stack.canRedo());

    // Undo all the way back to the empty state.
    REQUIRE(stack.undo());
    REQUIRE(stack.undo());
    REQUIRE(stack.undo());
    REQUIRE_FALSE(stack.canUndo());
    // Nothing further to undo.
    REQUIRE_FALSE(stack.undo());

    // Redo all the way forward again.
    REQUIRE(stack.redo());
    REQUIRE(stack.redo());
    REQUIRE(stack.redo());
    REQUIRE_FALSE(stack.canRedo());
    // Nothing further to redo.
    REQUIRE_FALSE(stack.redo());
}

TEST_CASE("UndoStack pushing a new edit clears the redo branch", "[undo]") {
    lrc::UndoStack<std::string> stack;
    stack.push("a");
    stack.push("b");
    REQUIRE(stack.undo());  // redo now holds "b", undo holds "a"
    REQUIRE(stack.canRedo());

    // A new edit forks the history: the old redo branch ("b") is discarded.
    stack.push("c");
    REQUIRE_FALSE(stack.canRedo());

    // Undo back to the empty state...
    REQUIRE(stack.undo());  // -> {a}
    REQUIRE(stack.undo());  // -> {}
    REQUIRE_FALSE(stack.canUndo());

    // ...and only "a" and "c" are redoable ("b" was dropped).
    REQUIRE(stack.redo());
    REQUIRE(stack.redo());
    REQUIRE_FALSE(stack.canRedo());
}

TEST_CASE("UndoStack discards the oldest state beyond the limit", "[undo]") {
    // limit 3, push 5 -> only "s3", "s4", "s5" survive.
    lrc::UndoStack<std::string> stack(3);
    stack.push("s1");
    stack.push("s2");
    stack.push("s3");
    stack.push("s4");
    stack.push("s5");

    // Exactly three undos remain: s1 and s2 were dropped.
    REQUIRE(stack.undo());
    REQUIRE(stack.undo());
    REQUIRE(stack.undo());
    REQUIRE_FALSE(stack.canUndo());

    // The three survivors round-trip through redo.
    REQUIRE(stack.redo());
    REQUIRE(stack.redo());
    REQUIRE(stack.redo());
    REQUIRE_FALSE(stack.canRedo());
}

TEST_CASE("UndoStack default limit keeps the newest 50 states", "[undo]") {
    lrc::UndoStack<int> stack;  // default limit is 50
    for (int i = 0; i < 60; ++i) {
        stack.push(i);
    }
    int undos = 0;
    while (stack.undo()) {
        ++undos;
    }
    REQUIRE(undos == 50);
    REQUIRE_FALSE(stack.canUndo());
}

TEST_CASE("UndoStack::top exposes the most recent state", "[undo]") {
    lrc::UndoStack<std::string> stack;
    stack.push("A");
    stack.push("B");
    REQUIRE(stack.top() == "B");
    REQUIRE(stack.undo());
    REQUIRE(stack.top() == "A");
    REQUIRE(stack.redo());
    REQUIRE(stack.top() == "B");
}

TEST_CASE("UndoStack::clear empties both stacks", "[undo]") {
    lrc::UndoStack<int> stack;
    stack.push(1);
    stack.push(2);
    stack.push(3);
    stack.undo();
    stack.undo();
    REQUIRE(stack.canUndo());
    REQUIRE(stack.canRedo());

    stack.clear();
    REQUIRE_FALSE(stack.canUndo());
    REQUIRE_FALSE(stack.canRedo());
    REQUIRE_FALSE(stack.undo());
    REQUIRE_FALSE(stack.redo());
}

TEST_CASE("UndoStack::setLimit takes effect dynamically", "[undo]") {
    lrc::UndoStack<std::string> stack(100);
    stack.push("a");
    stack.push("b");
    stack.push("c");
    stack.push("d");

    // Shrinking the limit drops the oldest states ("a", "b") immediately.
    stack.setLimit(2);
    REQUIRE(stack.undo());  // -> {c}
    REQUIRE(stack.undo());  // -> {}
    REQUIRE_FALSE(stack.canUndo());

    // Growing the limit again does not restore dropped history but accepts
    // new edits normally.
    stack.setLimit(10);
    REQUIRE_FALSE(stack.canUndo());
    stack.push("x");
    REQUIRE(stack.canUndo());
}

TEST_CASE("UndoStack works with a small value type", "[undo]") {
    struct Edit {
        int id;
        double amount;
    };
    lrc::UndoStack<Edit> stack;
    stack.push({1, 0.5});
    stack.push({2, -0.25});
    REQUIRE(stack.canUndo());
    REQUIRE(stack.undo());
    REQUIRE(stack.canRedo());
    REQUIRE(stack.redo());
    REQUIRE(stack.canUndo());
}
