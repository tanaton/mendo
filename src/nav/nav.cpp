#include "nav.h"
#include "ascii_util.h"
#include <cassert>
#include <charconv>
#include <ranges>
#include <utility>

std::optional<NavEntry> NavHistory::PushCapped(std::pmr::deque<NavEntry>& stack, NavEntry e)
{
    stack.push_back(std::move(e));
    if (stack.size() <= max_history_) {
        return std::nullopt;
    }
    std::optional<NavEntry> dropped{ std::move(stack.front()) };
    stack.pop_front();
    return dropped;
}

void NavHistory::Push(NavEntry current)
{
    pending_.clear();
    PushCapped(back_stack_, std::move(current));
    forward_stack_.clear();
}

void NavHistory::PushPending(NavEntry current)
{
    RollbackPending();
    PendingOp& op = pending_.emplace_back(PendingOp{ .to_back = true, .taken = std::exchange(forward_stack_, {}) });
    op.dropped = PushCapped(back_stack_, std::move(current));
}

void NavHistory::CommitPending() noexcept
{
    pending_.clear();
}

void NavHistory::RollbackPending()
{
    for (; !pending_.empty(); pending_.pop_back()) {
        PendingOp& op = pending_.back();
        auto& to = op.to_back ? back_stack_ : forward_stack_;
        auto& from = op.to_back ? forward_stack_ : back_stack_;
        to.pop_back();
        if (op.dropped) {
            to.push_front(std::move(*op.dropped));
        }
        from.append_range(op.taken | std::views::as_rvalue);
    }
}

bool NavHistory::Transfer(std::pmr::deque<NavEntry>& from, std::pmr::deque<NavEntry>& to, NavEntry current, NavEntry& out)
{
    // 表示中の文書は確定前の移動先へまだ切り替わっていないので、続けて移動するときは移動先を起点にする。
    // current のまま積むと表示中の位置が履歴に重複する。
    if (!pending_.empty()) {
        if (pending_.back().transfer) {
            current = pending_.back().taken.back();
        }
        else {
            // 開きかけのファイルは取り消し、表示中の文書から移動し直す。移動できなければ元に戻す。
            NavEntry pushed = std::move(back_stack_.back());
            RollbackPending();
            if (from.empty()) {
                PushPending(std::move(pushed));
                return false;
            }
        }
    }
    if (from.empty()) {
        return false;
    }

    // Push は進む履歴を空にして積むので back + forward は上限を超えず、移動で押し出しは起きない。
    assert(back_stack_.size() + forward_stack_.size() <= max_history_);
    PendingOp& op = pending_.emplace_back(PendingOp{ .to_back = &to == &back_stack_, .transfer = true });
    to.push_back(std::move(current));
    out = std::move(from.back());
    from.pop_back();
    op.taken.push_back(out);
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

// 外部で開く・HTML に残してよいリンクのスキームか。file:// などは通さない。
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
        if (s[i] == '%' && i + 2 < s.size() && std::from_chars(s.data() + i + 1, s.data() + i + 3, byte, 16).ptr == s.data() + i + 3) {
            out += static_cast<char>(byte);
            i += 2;
            continue;
        }
        out += s[i];
    }
    return out;
}

namespace {

// ShellExecuteW は関連付けのコマンド ("%1" を引用符で囲むだけの形式も多い) に URL を差し込むので、
// 引用符を閉じて引数を足せる " と、区切りになる空白・制御文字を %XX にする。URL としての意味は変わらない。
std::pmr::string EncodeShellUnsafeChars(std::string_view url)
{
    constexpr char kHex[] = "0123456789ABCDEF";
    std::pmr::string out;
    out.reserve(url.size());
    for (const char ch : url) {
        const auto c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c == 0x7F || c == '"') {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
        else {
            out += ch;
        }
    }
    return out;
}

} // namespace

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
    result.target = EncodeShellUnsafeChars(url);
    return result;
}
