#include "nav.h"
#include "ascii_util.h"

void NavHistory::PushCapped(std::pmr::deque<NavEntry>& stack, const NavEntry& e)
{
    stack.push_back(e);
    if (stack.size() > max_history_) {
        stack.pop_front();
    }
}

void NavHistory::Push(const NavEntry& current)
{
    PushCapped(back_stack_, current);
    forward_stack_.clear();
}

bool NavHistory::Transfer(std::pmr::deque<NavEntry>& from, std::pmr::deque<NavEntry>& to, const NavEntry& current, NavEntry& out)
{
    if (from.empty()) {
        return false;
    }

    PushCapped(to, current);
    out = std::move(from.back());
    from.pop_back();
    return true;
}

bool NavHistory::GoBack(const NavEntry& current, NavEntry& out)
{
    return Transfer(back_stack_, forward_stack_, current, out);
}

bool NavHistory::GoForward(const NavEntry& current, NavEntry& out)
{
    return Transfer(forward_stack_, back_stack_, current, out);
}

void NavHistory::Clear() noexcept
{
    back_stack_.clear();
    forward_stack_.clear();
}

// ShellExecuteWに渡しても安全なURLスキームかどうかを判定する。
// file:// やその他の危険なスキームをブロックし、http/https/mailto のみ許可する。
bool IsSafeUrlScheme(std::string_view url) noexcept
{
    return ascii_util::istarts_with(url, "http://") || ascii_util::istarts_with(url, "https://") || ascii_util::istarts_with(url, "mailto:");
}

LinkClickResult HandleLinkClick(std::string_view url)
{
    LinkClickResult result;
    if (url.empty()) {
        return result;
    }
    // 内部アンカーリンク: #something
    if (url[0] == '#') {
        result.type = LinkClickResult::Type::Anchor;
        result.target.assign(url.substr(1));
        return result;
    }
    // 安全なスキームの外部リンクのみ許可
    if (!IsSafeUrlScheme(url)) {
        return result;
    }
    result.type = LinkClickResult::Type::ExternalUrl;
    result.target.assign(url);
    return result;
}
