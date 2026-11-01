#include "nav.h"
#include <cassert>
#include <tuple>
#include <utility>

void NavHistory::PushCapped(std::deque<NavEntry>& stack, NavEntry e)
{
    stack.push_back(std::move(e));
    if (stack.size() > MAX_HISTORY) {
        stack.pop_front();
    }
}

void NavHistory::Push(NavEntry current)
{
    CommitPending();
    PushCapped(back_stack_, std::move(current));
    forward_stack_.clear();
}

void NavHistory::PushPending(NavEntry current)
{
    RollbackPending();
    saved_.emplace(back_stack_, forward_stack_);
    PushCapped(back_stack_, std::move(current));
    forward_stack_.clear();
}

void NavHistory::CommitPending() noexcept
{
    saved_.reset();
    chained_from_.reset();
}

void NavHistory::RollbackPending()
{
    if (saved_) {
        std::tie(back_stack_, forward_stack_) = std::move(*std::exchange(saved_, std::nullopt));
    }
    chained_from_.reset();
}

bool NavHistory::Transfer(std::deque<NavEntry>& from, std::deque<NavEntry>& to, NavEntry current, NavEntry& out)
{
    // 開きかけのファイルは取り消し、表示中の文書から移動し直す。移動できなければ元に戻す。
    if (saved_ && !chained_from_) {
        NavEntry pushed = back_stack_.back();
        RollbackPending();
        if (from.empty()) {
            PushPending(std::move(pushed));
            return false;
        }
    }
    if (from.empty()) {
        return false;
    }
    if (!saved_) {
        saved_.emplace(back_stack_, forward_stack_);
    }
    // Push は進む履歴を空にして積むので back + forward は上限を超えず、移動で押し出しは起きない。
    assert(back_stack_.size() + forward_stack_.size() <= MAX_HISTORY);
    // 表示中の文書は確定前の移動先へまだ切り替わっていないので、続けて移動するときは移動先を起点にする。
    // current のまま積むと表示中の位置が履歴に重複する。
    to.push_back(chained_from_ ? std::move(*chained_from_) : std::move(current));
    out = std::move(from.back());
    from.pop_back();
    chained_from_ = out;
    return true;
}

bool NavHistory::GoBack(NavEntry current, NavEntry& out)
{
    return Transfer(back_stack_, forward_stack_, std::move(current), out);
}

bool NavHistory::GoForward(NavEntry current, NavEntry& out)
{
    return Transfer(forward_stack_, back_stack_, std::move(current), out);
}
