#include "nav.h"
#include <algorithm>
#include <cstdlib>
#include <utility>

void NavHistory::PushCapped(std::deque<NavEntry>& stack, NavEntry e)
{
    stack.push_back(std::move(e));
    if (stack.size() > MAX_HISTORY) {
        stack.pop_front();
    }
}

int NavHistory::PendingSteps() const noexcept
{
    const auto* traverse = std::get_if<PendingTraverse>(&pending_);
    return traverse ? traverse->steps : 0;
}

size_t NavHistory::BackSize() const noexcept
{
    if (std::holds_alternative<PendingOpen>(pending_)) {
        return std::min(back_stack_.size() + 1, MAX_HISTORY);
    }
    return static_cast<size_t>(static_cast<ptrdiff_t>(back_stack_.size()) + PendingSteps());
}

size_t NavHistory::ForwardSize() const noexcept
{
    if (std::holds_alternative<PendingOpen>(pending_)) {
        return 0;
    }
    return static_cast<size_t>(static_cast<ptrdiff_t>(forward_stack_.size()) - PendingSteps());
}

void NavHistory::Push(NavEntry current)
{
    if (auto* traverse = std::get_if<PendingTraverse>(&pending_)) {
        if (traverse->steps > 0) {
            // 移動先は進む履歴にあり、この移動で消える。別のファイルを開くのと同じにする。
            pending_ = PendingOpen{ std::move(traverse->left) };
        }
        else if (static_cast<size_t>(-traverse->steps) == MAX_HISTORY) {
            // 移動先が最古の履歴で、積むと押し出されるので、移動元は積まない。
            forward_stack_.clear();
            return;
        }
        else {
            --traverse->steps;
        }
    }
    PushCapped(back_stack_, std::move(current));
    forward_stack_.clear();
}

void NavHistory::PushPending(NavEntry current)
{
    pending_ = PendingOpen{ std::move(current) };
}

void NavHistory::CommitOpened(NavEntry left)
{
    if (std::holds_alternative<PendingOpen>(pending_)) {
        PushCapped(back_stack_, std::move(left));
        forward_stack_.clear();
    }
    else if (const auto* traverse = std::get_if<PendingTraverse>(&pending_)) {
        // 表示していた文書から移動先の手前までを、移動した向きと反対のスタックへ移す。移動先は表示中になるので外す。
        // 移した数だけ外すので、上限は超えない。
        auto& from = traverse->steps < 0 ? back_stack_ : forward_stack_;
        auto& to = traverse->steps < 0 ? forward_stack_ : back_stack_;
        to.push_back(std::move(left));
        for (int i = std::abs(traverse->steps); i > 1; --i) {
            to.push_back(std::move(from.back()));
            from.pop_back();
        }
        from.pop_back();
    }
    pending_ = Settled{};
}

void NavHistory::CommitPending()
{
    if (auto* open = std::get_if<PendingOpen>(&pending_)) {
        CommitOpened(std::move(open->left));
    }
    else if (auto* traverse = std::get_if<PendingTraverse>(&pending_)) {
        CommitOpened(std::move(traverse->left));
    }
}

void NavHistory::RollbackPending() noexcept
{
    pending_ = Settled{};
}

bool NavHistory::Move(int direction, NavEntry current, NavEntry& out)
{
    // 開きかけのファイルは取り消し、表示中の文書から移動する (移動できなければ開くのを続ける)。戻る/進むを続けるときは、
    // 表示中の文書はまだ確定前の移動先へ切り替わっていないので、移動先を起点にする。
    const int steps = PendingSteps() + direction;
    if (steps == 0) {
        // 表示中の文書へ戻った。読み込まないので確定し、今の位置に留まる。
        out = std::move(current);
        pending_ = Settled{};
        return true;
    }
    const auto& stack = steps < 0 ? back_stack_ : forward_stack_;
    const auto distance = static_cast<size_t>(std::abs(steps));
    if (distance > stack.size()) {
        return false;
    }
    out = stack[stack.size() - distance];
    pending_ = PendingTraverse{ steps, std::move(current) };
    return true;
}

bool NavHistory::GoBack(NavEntry current, NavEntry& out)
{
    return Move(-1, std::move(current), out);
}

bool NavHistory::GoForward(NavEntry current, NavEntry& out)
{
    return Move(1, std::move(current), out);
}
