#include "nav.h"
#include <cassert>
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
    if (auto* open = std::get_if<PendingOpen>(&pending_)) {
        NavEntry left = std::move(back_stack_.back());
        PushCapped(open->saved.back, std::move(current));
        open->saved.forward.clear();
        back_stack_ = open->saved.back;
        PushCapped(back_stack_, std::move(left));
        return;
    }
    if (auto* traverse = std::get_if<PendingTraverse>(&pending_)) {
        PushWhileTraversing(*traverse, std::move(current));
        return;
    }
    PushCapped(back_stack_, std::move(current));
    forward_stack_.clear();
}

void NavHistory::PushWhileTraversing(PendingTraverse& t, NavEntry current)
{
    const size_t left_index = LeftIndex(t);
    PushCapped(t.saved.back, current);
    t.saved.forward.clear();
    if (t.forward_first) {
        // 移動先は表示中の文書より先にあり、表示中の文書の中の移動で進む履歴ごと消える。別のファイルを開くのと同じにする。
        NavEntry left = std::move(back_stack_[left_index]);
        back_stack_ = t.saved.back;
        PushCapped(back_stack_, std::move(left));
        forward_stack_.clear();
        pending_ = PendingOpen{ std::move(t.saved) };
        return;
    }
    // 移動先は表示中の文書より前にある。表示中の文書の位置より先の進む履歴を消し、その手前に移動元を挟む。
    forward_stack_.erase(forward_stack_.begin(), forward_stack_.begin() + static_cast<ptrdiff_t>(left_index));
    if (back_stack_.size() + forward_stack_.size() >= MAX_HISTORY) {
        // 最古の履歴は移動先そのもので捨てられないので、移動元を積まない。
        if (back_stack_.empty()) {
            return;
        }
        back_stack_.pop_front();
    }
    forward_stack_.insert(forward_stack_.begin() + 1, std::move(current));
}

void NavHistory::PushPending(NavEntry current)
{
    RollbackPending();
    // 進む履歴はこの後空にするので、写さずに渡す。
    Stacks saved{ back_stack_, std::move(forward_stack_) };
    pending_ = PendingOpen{ std::move(saved) };
    PushCapped(back_stack_, std::move(current));
    forward_stack_.clear();
}

void NavHistory::CommitOpened(NavEntry left)
{
    if (std::holds_alternative<PendingOpen>(pending_)) {
        back_stack_.back() = std::move(left);
    }
    else if (const auto* traverse = std::get_if<PendingTraverse>(&pending_)) {
        LeftStack(*traverse)[LeftIndex(*traverse)] = std::move(left);
    }
    pending_ = Settled{};
}

void NavHistory::CommitPending() noexcept
{
    pending_ = Settled{};
}

void NavHistory::RollbackPending()
{
    Stacks* saved = nullptr;
    if (auto* open = std::get_if<PendingOpen>(&pending_)) {
        saved = &open->saved;
    }
    else if (auto* traverse = std::get_if<PendingTraverse>(&pending_)) {
        saved = &traverse->saved;
    }
    if (saved) {
        back_stack_ = std::move(saved->back);
        forward_stack_ = std::move(saved->forward);
    }
    pending_ = Settled{};
}

bool NavHistory::Transfer(std::deque<NavEntry>& from, std::deque<NavEntry>& to, NavEntry current, NavEntry& out)
{
    // 開きかけのファイルは取り消し、表示中の文書から移動し直す。移動できなければ元に戻す。
    if (std::holds_alternative<PendingOpen>(pending_)) {
        RollbackPending();
        if (from.empty()) {
            PushPending(std::move(current));
            return false;
        }
    }
    if (from.empty()) {
        return false;
    }
    // Push は進む履歴を空にして積むので back + forward は上限を超えず、移動で押し出しは起きない。
    assert(back_stack_.size() + forward_stack_.size() <= MAX_HISTORY);
    PendingTraverse next;
    if (auto* traverse = std::get_if<PendingTraverse>(&pending_)) {
        // 表示中の文書は確定前の移動先へまだ切り替わっていないので、続けて移動するときは移動先を起点にする。
        // current のまま積むと表示中の位置が履歴に重複する。表示中の文書の位置は今の位置にする。
        LeftStack(*traverse)[LeftIndex(*traverse)] = std::move(current);
        next = std::move(*traverse);
        current = std::move(next.to);
    }
    else {
        next.saved = { back_stack_, forward_stack_ };
        next.forward_first = &to == &back_stack_;
    }
    // 表示中の文書の位置まで戻ったら、読み込まないので確定する。
    const bool returns_to_left = &from == &LeftStack(next) && from.size() - 1 == LeftIndex(next);
    to.push_back(std::move(current));
    out = std::move(from.back());
    from.pop_back();
    if (returns_to_left) {
        pending_ = Settled{};
        return true;
    }
    next.to = out;
    pending_ = std::move(next);
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
