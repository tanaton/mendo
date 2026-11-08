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
    CommitPending();
    PushCapped(back_stack_, std::move(current));
    forward_stack_.clear();
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
    // Push は進む履歴を空にして積むので back + forward は上限を超えず、移動で押し出しは起きない。
    assert(back_stack_.size() + forward_stack_.size() <= MAX_HISTORY);
    PendingTraverse next;
    if (auto* traverse = std::get_if<PendingTraverse>(&pending_)) {
        // 表示中の文書は確定前の移動先へまだ切り替わっていないので、続けて移動するときは移動先を起点にする。
        // current のまま積むと表示中の位置が履歴に重複する。
        next.saved = std::move(traverse->saved);
        current = std::move(traverse->to);
    }
    else {
        next.saved = { back_stack_, forward_stack_ };
    }
    to.push_back(std::move(current));
    out = std::move(from.back());
    from.pop_back();
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
