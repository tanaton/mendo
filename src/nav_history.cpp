#include "nav_history.h"

void NavHistory::Push(const NavEntry& current) {
    back_stack_.push_back(current);
    forward_stack_.clear();

    // Cap history size
    if (back_stack_.size() > MAX_HISTORY) {
        back_stack_.erase(back_stack_.begin());
    }
}

bool NavHistory::GoBack(const NavEntry& current, NavEntry& out) {
    if (back_stack_.empty()) return false;

    forward_stack_.push_back(current);
    out = back_stack_.back();
    back_stack_.pop_back();
    return true;
}

bool NavHistory::GoForward(const NavEntry& current, NavEntry& out) {
    if (forward_stack_.empty()) return false;

    back_stack_.push_back(current);
    out = forward_stack_.back();
    forward_stack_.pop_back();
    return true;
}

void NavHistory::Clear() {
    back_stack_.clear();
    forward_stack_.clear();
}
