#pragma once
#include <cstdint>
#include <deque>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// ファイルが編集されて絶対 y 座標が変わっても、同じノードの相対位置に戻れる。
struct NavEntry {
    std::pmr::wstring file_path;
    int node = -1;
    float offset = 0.0f;

    NavEntry() = default;
    NavEntry(std::wstring_view fp, int n, float off)
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

    void Push(NavEntry current);
    // PushPending と GoBack/GoForward は、ロード結果で確定 (Commit) か取り消し (Rollback) をする。
    // 開けなかったファイルの分だけ履歴が重複したり、戻る/進むの位置がずれたりするのを防ぐ。
    // Push や Clear が挟まった後の Rollback は何もしない。
    // ロード中に次の操作が来たら、ロード中の文書はまだ表示されていないので、
    // PushPending は前の仮の操作を取り消してから積み直し、戻る/進むの連続はロード中の移動先を起点に続ける。
    // 続けた操作は Rollback でまとめて取り消す。
    void PushPending(NavEntry current);
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
    bool Transfer(std::pmr::deque<NavEntry>& from, std::pmr::deque<NavEntry>& to, NavEntry current, NavEntry& out);
    // 上限で押し出した最古のエントリを返す。
    std::optional<NavEntry> PushCapped(std::pmr::deque<NavEntry>& stack, NavEntry e);

    // taken: PushPending では進む履歴全体、Transfer では移動先の 1 件。dropped: PushPending が上限で押し出した分。
    struct PendingOp {
        bool to_back = false;
        bool transfer = false;
        std::pmr::deque<NavEntry> taken;
        std::optional<NavEntry> dropped;
    };

    std::pmr::deque<NavEntry> back_stack_;
    std::pmr::deque<NavEntry> forward_stack_;
    // 確定待ちの操作を古い順に持つ。
    std::pmr::vector<PendingOp> pending_;
    size_t max_history_ = MAX_HISTORY;
};

struct LinkClickResult {
    enum class Type : uint8_t {
        None,
        Anchor,
        ExternalUrl
    };
    Type type = Type::None;
    // Anchor: アンカー名 (UTF-8、先頭 '#' を除きパーセントデコード済み)。NavigateAnchorAction にそのまま渡せる。
    // ブラウザのアドレスバーなどからコピーしたリンクはエンコードされている。
    // ExternalUrl: URL (UTF-8)。ShellOpen に渡す直前で wstring 化する。
    std::pmr::string target;
};

LinkClickResult ClassifyLinkClick(std::string_view url);
bool IsSafeUrlScheme(std::string_view url) noexcept;
// md4c は URL をエンコードされたまま渡すので、ファイルパスやアンカーとして引く前に通す。
// 16 進 2 桁が続かない % はそのまま残す。
std::pmr::string PercentDecode(std::string_view s);
