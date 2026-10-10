#pragma once
#include <cstdint>
#include <deque>
#include <memory_resource>
#include <string>
#include <string_view>

// ファイルが編集されて絶対 y 座標が変わっても、同じノードの相対位置に戻れる。
struct NavEntry {
    std::pmr::wstring file_path;
    int node = -1;
    float offset = 0.0f;

    NavEntry() = default;
    NavEntry(std::wstring_view fp, int n = -1, float off = 0.0f)
        : file_path(fp), node(n), offset(off)
    {
    }
};

class NavHistory {
public:
    static constexpr size_t MAX_HISTORY = 1024;

    NavHistory() = default;
    // 上限はテストで容量超過経路を少ない操作数で踏むために差し替え可能にしている。
    explicit NavHistory(size_t max_history)
        : max_history_(max_history)
    {
    }

    void Push(const NavEntry& current);
    bool GoBack(const NavEntry& current, NavEntry& out);
    bool GoForward(const NavEntry& current, NavEntry& out);

    bool CanGoBack() const noexcept
    {
        return !back_stack_.empty();
    }
    bool CanGoForward() const noexcept
    {
        return !forward_stack_.empty();
    }

    size_t BackSize() const noexcept
    {
        return back_stack_.size();
    }
    size_t ForwardSize() const noexcept
    {
        return forward_stack_.size();
    }

    void Clear() noexcept;

private:
    // GoBack/GoForward の対称処理を集約する。
    bool Transfer(std::pmr::deque<NavEntry>& from, std::pmr::deque<NavEntry>& to, const NavEntry& current, NavEntry& out);
    void PushCapped(std::pmr::deque<NavEntry>& stack, const NavEntry& e);

    std::pmr::deque<NavEntry> back_stack_;
    std::pmr::deque<NavEntry> forward_stack_;
    size_t max_history_ = MAX_HISTORY;
};

struct LinkClickResult {
    enum class Type : uint8_t {
        None,
        Anchor,
        ExternalUrl
    };
    Type type = Type::None;
    // Anchor: アンカー名 (UTF-8、先頭 '#' を除いた部分)。NavigateAnchorAction にそのまま渡せる。
    // ExternalUrl: URL (UTF-8)。ShellOpen に渡す直前で wstring 化する。
    std::pmr::string target;
};

LinkClickResult HandleLinkClick(std::string_view url);
bool IsSafeUrlScheme(std::string_view url) noexcept;
