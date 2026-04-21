#pragma once
#include <string>
#include <string_view>
#include <deque>
#include <memory_resource>
#include <map>

// 単一のナビゲーション履歴エントリ: ファイルパス + スクロール位置（ノード単位）。
// ノードインデックスと、そのノードの y_position からのオフセットで位置を表現する。
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
    // 内部エントリ: パスインデックス + ノードインデックス + オフセット（12バイト）
    struct InternalEntry {
        uint32_t path_index = 0;
        int node = -1;
        float offset = 0.0f;
    };

    // パスをインターン化し、インデックスを返す
    uint32_t InternPath(std::wstring_view path);

    // NavEntry ↔ InternalEntry 変換
    InternalEntry ToInternal(const NavEntry& e) { return { InternPath(e.file_path), e.node, e.offset }; }
    NavEntry ToExternal(const InternalEntry& e) const;

    // path_pool_ は deque にして emplace_back での参照安定性を保証し、
    // path_index_ のキー (std::wstring_view) が pool 内の文字列を指し続けるようにする。
    // 典型的なセッションのエントリ数は数十〜数百で、文字列ハッシュの計算コストと
    // バケット配列のキャッシュミスを避けられる std::pmr::map のほうが優位。
    std::pmr::deque<std::pmr::wstring> path_pool_;
    std::pmr::map<std::wstring_view, uint32_t> path_index_;
    std::pmr::deque<InternalEntry> back_stack_;
    std::pmr::deque<InternalEntry> forward_stack_;

    // Back→Forward→Back の連続操作で path_index_::find を回避する直前値キャッシュ
    std::wstring_view last_interned_view_;
    uint32_t last_interned_index_ = UINT32_MAX;
};
