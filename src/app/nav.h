#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <variant>

// ファイルが編集されて絶対 y 座標が変わっても、同じノードの相対位置に戻れる。
struct NavEntry {
    std::wstring file_path;
    int node = -1;
    float offset = 0.0f;
};

class NavHistory {
public:
    static constexpr size_t MAX_HISTORY = 1024;

    // 表示中の文書の中の移動。別の文書を読み込んでいる間は、読み込みより前の移動として積む
    // (取り消しても残り、読み込みが終われば表示していた文書の位置がその後に入る)。
    void Push(NavEntry current);
    // PushPending と GoBack/GoForward は、ロード結果で確定 (CommitOpened / CommitPending) か取り消し (Rollback) をする。
    // 確定するまで履歴は変えないので、開けなかったファイルの分だけ履歴が重複したり、戻る/進むの位置がずれたりしない。
    // ロード中に次の操作が来たら、ロード中の文書はまだ表示されていないので、
    // PushPending は前の仮の操作を置き換え、戻る/進むの連続はロード中の移動先を起点に続ける。
    // 続けた操作は Rollback でまとめて取り消す。
    void PushPending(NavEntry current);
    // 別の文書を開き終えた。表示していた文書は、差し替えた時点の位置 left で履歴に入る。
    void CommitOpened(NavEntry left);
    // 文書を差し替えずに確定する (戻る/進むで表示中の文書の中の位置へ移った)。表示していた文書は最後に操作した時点の位置で入る。
    void CommitPending();
    void RollbackPending() noexcept;
    bool GoBack(NavEntry current, NavEntry& out);
    bool GoForward(NavEntry current, NavEntry& out);

    // 以下は確定待ちの操作を確定したときの数。
    bool CanGoBack() const noexcept
    {
        return BackSize() > 0;
    }
    bool CanGoForward() const noexcept
    {
        return ForwardSize() > 0;
    }
    size_t BackSize() const noexcept;
    size_t ForwardSize() const noexcept;

private:
    // direction は戻るなら -1、進むなら 1。
    bool Move(int direction, NavEntry current, NavEntry& out);
    // 上限を超えたら最古のエントリを捨てる。
    void PushCapped(std::deque<NavEntry>& stack, NavEntry e);
    int PendingSteps() const noexcept;

    // 確定した履歴。
    std::deque<NavEntry> back_stack_;
    std::deque<NavEntry> forward_stack_;

    struct Settled {};
    // 別のファイルを開いている (PushPending)。left は表示中の文書の、開くと決めた時点の位置。
    struct PendingOpen {
        NavEntry left;
    };
    // 戻る/進むで移動している。steps は確定した履歴の上での移動数で、負は戻る (0 にはならない)。
    // left は表示中の文書の、最後に押した時点の位置。
    struct PendingTraverse {
        int steps = 0;
        NavEntry left;
    };
    std::variant<Settled, PendingOpen, PendingTraverse> pending_;
};
