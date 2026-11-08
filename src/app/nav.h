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
    // 開けなかったファイルの分だけ履歴が重複したり、戻る/進むの位置がずれたりするのを防ぐ。
    // ロード中に次の操作が来たら、ロード中の文書はまだ表示されていないので、
    // PushPending は前の仮の操作を取り消してから積み直し、戻る/進むの連続はロード中の移動先を起点に続ける。
    // 続けた操作は Rollback でまとめて取り消す。
    void PushPending(NavEntry current);
    // 別の文書を開き終えた。表示していた文書は、差し替えた時点の位置 left で履歴に入る。
    void CommitOpened(NavEntry left);
    // 文書を差し替えずに確定する (戻る/進むで表示中の文書へ戻った)。
    void CommitPending() noexcept;
    void RollbackPending();
    bool GoBack(NavEntry current, NavEntry& out);
    bool GoForward(NavEntry current, NavEntry& out);

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

private:
    // GoBack/GoForward の対称処理を集約する。
    bool Transfer(std::deque<NavEntry>& from, std::deque<NavEntry>& to, NavEntry current, NavEntry& out);
    // 上限を超えたら最古のエントリを捨てる。
    void PushCapped(std::deque<NavEntry>& stack, NavEntry e);

    std::deque<NavEntry> back_stack_;
    std::deque<NavEntry> forward_stack_;

    // 確定待ちの最初の操作の直前の両スタック。取り消しでそのまま戻す (上限で押し出した分も戻る)。
    struct Stacks {
        std::deque<NavEntry> back;
        std::deque<NavEntry> forward;
    };
    struct Settled {};
    // 別のファイルを開いている (PushPending)。back_stack_ の末尾が表示中の文書の位置。
    struct PendingOpen {
        Stacks saved;
    };
    // 戻る/進むで移動している。to は確定待ちの移動先で、続く戻る/進むの起点になる。
    // 表示中の文書の位置は、最初の移動が進むなら back_stack_ の、戻るなら forward_stack_ の saved.back / saved.forward の
    // 長さの位置にある。続く移動はその上に積むか取り出すので動かない。
    struct PendingTraverse {
        Stacks saved;
        NavEntry to;
        bool forward_first = false;
    };
    std::deque<NavEntry>& LeftStack(const PendingTraverse& t) noexcept
    {
        return t.forward_first ? back_stack_ : forward_stack_;
    }
    static size_t LeftIndex(const PendingTraverse& t) noexcept
    {
        return (t.forward_first ? t.saved.back : t.saved.forward).size();
    }
    void PushWhileTraversing(PendingTraverse& t, NavEntry current);

    std::variant<Settled, PendingOpen, PendingTraverse> pending_;
};
