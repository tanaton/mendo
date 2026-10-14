#include "nav.h"
#include "ascii_util.h"
#include <cassert>
#include <iterator>
#include <utility>

std::optional<NavEntry> NavHistory::PushCapped(std::pmr::deque<NavEntry>& stack, const NavEntry& e)
{
    stack.push_back(e);
    if (stack.size() <= max_history_) {
        return std::nullopt;
    }
    std::optional<NavEntry> dropped{ std::move(stack.front()) };
    stack.pop_front();
    return dropped;
}

void NavHistory::Push(const NavEntry& current)
{
    pending_.reset();
    PushCapped(back_stack_, current);
    forward_stack_.clear();
}

void NavHistory::PushPending(const NavEntry& current)
{
    pending_ = PendingOp{ .to_back = true, .taken = std::exchange(forward_stack_, {}) };
    pending_->dropped = PushCapped(back_stack_, current);
}

void NavHistory::CommitPending() noexcept
{
    pending_.reset();
}

void NavHistory::RollbackPending()
{
    if (!pending_) {
        return;
    }
    auto& to = pending_->to_back ? back_stack_ : forward_stack_;
    auto& from = pending_->to_back ? forward_stack_ : back_stack_;
    to.pop_back();
    if (pending_->dropped) {
        to.push_front(std::move(*pending_->dropped));
    }
    from.insert(from.end(), std::make_move_iterator(pending_->taken.begin()), std::make_move_iterator(pending_->taken.end()));
    pending_.reset();
}

bool NavHistory::Transfer(std::pmr::deque<NavEntry>& from, std::pmr::deque<NavEntry>& to, const NavEntry& current, NavEntry& out)
{
    if (from.empty()) {
        return false;
    }

    // Push は進む履歴を空にして積むので back + forward は上限を超えず、移動で押し出しは起きない。
    assert(back_stack_.size() + forward_stack_.size() <= max_history_);
    pending_ = PendingOp{ .to_back = &to == &back_stack_ };
    to.push_back(current);
    out = std::move(from.back());
    from.pop_back();
    pending_->taken.push_back(out);
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
    pending_.reset();
}

// ShellExecuteWに渡しても安全なURLスキームかどうかを判定する。
// file:// やその他の危険なスキームをブロックし、http/https/mailto のみ許可する。
bool IsSafeUrlScheme(std::string_view url) noexcept
{
    return ascii_util::istarts_with(url, "http://") || ascii_util::istarts_with(url, "https://") || ascii_util::istarts_with(url, "mailto:");
}

std::pmr::string PercentDecode(std::string_view s)
{
    std::pmr::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned int byte = 0;
        if (s[i] == '%' && i + 2 < s.size() && ascii_util::from_chars(s.data() + i + 1, 2, byte, 16u) == s.data() + i + 3) {
            out += static_cast<char>(byte);
            i += 2;
            continue;
        }
        out += s[i];
    }
    return out;
}

LinkClickResult HandleLinkClick(std::string_view url)
{
    LinkClickResult result;
    if (url.empty()) {
        return result;
    }
    if (url[0] == '#') {
        result.type = LinkClickResult::Type::Anchor;
        result.target = PercentDecode(url.substr(1));
        return result;
    }
    if (!IsSafeUrlScheme(url)) {
        return result;
    }
    result.type = LinkClickResult::Type::ExternalUrl;
    result.target.assign(url);
    return result;
}
