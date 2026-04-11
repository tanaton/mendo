#include "nav_history.h"

uint16_t NavHistory::InternPath(std::wstring_view path)
{
    for (uint16_t i = 0; i < path_pool_.size(); ++i) {
        if (path_pool_[i] == path) {
            return i;
        }
    }
    path_pool_.emplace_back(path);
    return static_cast<uint16_t>(path_pool_.size() - 1);
}

NavEntry NavHistory::ToExternal(const InternalEntry& e) const
{
    return NavEntry(path_pool_[e.path_index], e.scroll_y);
}

void NavHistory::Push(const NavEntry& current)
{
    back_stack_.emplace_back(ToInternal(current));
    forward_stack_.clear();

    // 履歴サイズを制限（dequeなのでpop_frontはO(1)）
    if (back_stack_.size() > MAX_HISTORY) {
        back_stack_.pop_front();
    }
}

bool NavHistory::GoBack(const NavEntry& current, NavEntry& out)
{
    if (back_stack_.empty()) {
        return false;
    }

    forward_stack_.emplace_back(ToInternal(current));
    if (forward_stack_.size() > MAX_HISTORY) {
        forward_stack_.pop_front();
    }
    out = ToExternal(back_stack_.back());
    back_stack_.pop_back();
    return true;
}

bool NavHistory::GoForward(const NavEntry& current, NavEntry& out)
{
    if (forward_stack_.empty()) {
        return false;
    }

    back_stack_.emplace_back(ToInternal(current));
    out = ToExternal(forward_stack_.back());
    forward_stack_.pop_back();
    return true;
}

void NavHistory::Clear() noexcept
{
    back_stack_.clear();
    forward_stack_.clear();
    path_pool_.clear();
}
