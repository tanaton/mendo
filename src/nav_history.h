#pragma once
#include <string>
#include <vector>

// A single navigation history entry: file path + scroll position.
struct NavEntry {
    std::wstring file_path;
    float scroll_y = 0.0f;
};

// Browser-style back/forward navigation history.
// Pure logic, no Win32 dependencies — fully testable.
class NavHistory {
public:
    // Record the current state before a navigation.
    // Pushes current onto back stack and clears forward stack.
    void Push(const NavEntry& current);

    // Navigate back: moves current to forward stack, pops from back stack.
    // Returns true and writes the entry to navigate to into `out`, or false if
    // there is nothing to go back to. `current` is the state right now (before going back).
    bool GoBack(const NavEntry& current, NavEntry& out);

    // Navigate forward: moves current to back stack, pops from forward stack.
    bool GoForward(const NavEntry& current, NavEntry& out);

    bool CanGoBack() const { return !back_stack_.empty(); }
    bool CanGoForward() const { return !forward_stack_.empty(); }

    size_t BackSize() const { return back_stack_.size(); }
    size_t ForwardSize() const { return forward_stack_.size(); }

    void Clear();

    static constexpr size_t MAX_HISTORY = 50;

private:
    std::vector<NavEntry> back_stack_;
    std::vector<NavEntry> forward_stack_;
};
