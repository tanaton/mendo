#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <memory_resource>

// 単一のナビゲーション履歴エントリ: ファイルパス + スクロール位置。
struct NavEntry {
    std::pmr::wstring file_path;
    float scroll_y = 0.0f;

    NavEntry() = default;
    NavEntry(std::wstring_view fp, float sy = 0.0f)
        : file_path(fp), scroll_y(sy) {}
};

// ブラウザスタイルの戻る/進むナビゲーション履歴。
// 純粋なロジックのみ、Win32依存なし — 完全にテスト可能。
class NavHistory {
public:
    // ナビゲーション前の現在の状態を記録する。
    // currentを戻るスタックにプッシュし、進むスタックをクリアする。
    void Push(const NavEntry& current);

    // 戻るナビゲーション: currentを進むスタックに移動し、戻るスタックからポップする。
    // ナビゲート先のエントリを`out`に書き込みtrueを返す。戻る先がない場合はfalseを返す。
    // `current`は現在の状態（戻る前の状態）。
    bool GoBack(const NavEntry& current, NavEntry& out);

    // 進むナビゲーション: currentを戻るスタックに移動し、進むスタックからポップする。
    bool GoForward(const NavEntry& current, NavEntry& out);

    constexpr bool CanGoBack() const noexcept { return !back_stack_.empty(); }
    constexpr bool CanGoForward() const noexcept { return !forward_stack_.empty(); }

    constexpr size_t BackSize() const noexcept { return back_stack_.size(); }
    constexpr size_t ForwardSize() const noexcept { return forward_stack_.size(); }

    void Clear() noexcept;

    static constexpr size_t MAX_HISTORY = 50;

private:
    std::pmr::vector<NavEntry> back_stack_;
    std::pmr::vector<NavEntry> forward_stack_;
};
