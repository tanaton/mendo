#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <memory_resource>

// A single navigation history entry: file path + scroll position.
struct NavEntry {
    std::pmr::wstring file_path;
    float scroll_y = 0.0f;

    NavEntry() = default;
    NavEntry(std::wstring_view fp, float sy = 0.0f)
        : file_path(fp), scroll_y(sy) {}
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

    bool CanGoBack() const noexcept { return !back_stack_.empty(); }
    bool CanGoForward() const noexcept { return !forward_stack_.empty(); }

    size_t BackSize() const noexcept { return back_stack_.size(); }
    size_t ForwardSize() const noexcept { return forward_stack_.size(); }

    void Clear() noexcept;

    static constexpr size_t MAX_HISTORY = 50;

private:
    std::pmr::vector<NavEntry> back_stack_;
    std::pmr::vector<NavEntry> forward_stack_;
};
