#pragma once

// スクロール位置の復元状態を管理する値型。
// セッション復元とナビゲーション履歴（戻る/進む）の両方をノードベースで統合する。
// 各フィールドは App の複数箇所から直接読み書きされるため public にしている。
struct ScrollRestoration {
    int pending_restore_node = -1;
    int pending_restore_offset = 0;

    bool HasNodeRestore() const noexcept { return pending_restore_node >= 0; }

    void SetNodeRestore(int node, int offset) noexcept
    {
        pending_restore_node = node;
        pending_restore_offset = offset;
    }

    void ClearNodeRestore() noexcept
    {
        pending_restore_node = -1;
        pending_restore_offset = 0;
    }

    void Reset() noexcept
    {
        pending_restore_node = -1;
        pending_restore_offset = 0;
    }
};
