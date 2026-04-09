#pragma once

// スクロール位置の復元状態を管理する値型。
// ナビゲーション時の遅延スクロールとセッション復元時のノードベース復元を統合する。
// 各フィールドは App の複数箇所から直接読み書きされるため public にしている。
struct ScrollRestoration {
    float pending_nav_scroll_y = -1.0f;
    int pending_restore_node = -1;
    int pending_restore_offset = 0;
    float pending_restore_scroll_y = -1.0f;  // 遅延レイアウト完了後に適用する生のscroll_y

    bool HasNavScroll() const noexcept { return pending_nav_scroll_y >= 0.0f; }
    bool HasNodeRestore() const noexcept { return pending_restore_node >= 0; }

    float ConsumeNavScroll() noexcept
    {
        const float v = pending_nav_scroll_y;
        pending_nav_scroll_y = -1.0f;
        return v;
    }

    void SetNodeRestore(int node, int offset, float scroll_y = -1.0f) noexcept
    {
        pending_restore_node = node;
        pending_restore_offset = offset;
        pending_restore_scroll_y = scroll_y;
    }

    void ClearNodeRestore() noexcept
    {
        pending_restore_node = -1;
        pending_restore_offset = 0;
    }

    void Reset() noexcept
    {
        pending_nav_scroll_y = -1.0f;
        pending_restore_node = -1;
        pending_restore_offset = 0;
        pending_restore_scroll_y = -1.0f;
    }
};
