#pragma once
#include <string>
#include <string_view>
#include <deque>
#include <memory_resource>

// 単一のナビゲーション履歴エントリ: ファイルパス + スクロール位置。
struct NavEntry {
    std::pmr::wstring file_path;
    float scroll_y = 0.0f;

    NavEntry() = default;
    NavEntry(std::wstring_view fp, float sy = 0.0f)
        : file_path(fp), scroll_y(sy)
    {
    }
};

// ブラウザスタイルの戻る/進むナビゲーション履歴。
// 純粋なロジックのみ、Win32依存なし — 完全にテスト可能。
// 内部ではパスをインターン化し、同一パスの重複保持を回避する。
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

    bool CanGoBack() const noexcept { return !back_stack_.empty(); }
    bool CanGoForward() const noexcept { return !forward_stack_.empty(); }

    size_t BackSize() const noexcept { return back_stack_.size(); }
    size_t ForwardSize() const noexcept { return forward_stack_.size(); }

    void Clear() noexcept;

    static constexpr size_t MAX_HISTORY = 1024;

private:
    // 内部エントリ: パスインデックス + スクロール位置（8バイト）
    struct InternalEntry {
        uint16_t path_index = 0;
        float scroll_y = 0.0f;
    };

    // パスをインターン化し、インデックスを返す
    uint16_t InternPath(std::wstring_view path);

    // NavEntry ↔ InternalEntry 変換
    InternalEntry ToInternal(const NavEntry& e) { return { InternPath(e.file_path), e.scroll_y }; }
    NavEntry ToExternal(const InternalEntry& e) const;

    std::pmr::vector<std::pmr::wstring> path_pool_;
    std::pmr::deque<InternalEntry> back_stack_;
    std::pmr::deque<InternalEntry> forward_stack_;
};
