#pragma once
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// ファイルが編集されて絶対 y 座標が変わっても、同じノードの相対位置に戻れる。
struct NavEntry {
    std::wstring file_path;
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
    // Push が挟まった後の Rollback は何もしない。
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
    bool Transfer(std::deque<NavEntry>& from, std::deque<NavEntry>& to, NavEntry current, NavEntry& out);
    // 上限を超えたら最古のエントリを捨てる。
    void PushCapped(std::deque<NavEntry>& stack, NavEntry e);

    std::deque<NavEntry> back_stack_;
    std::deque<NavEntry> forward_stack_;
    // 確定待ちの最初の操作の直前の両スタック。取り消しでそのまま戻す (上限で押し出した分も戻る)。
    std::optional<std::pair<std::deque<NavEntry>, std::deque<NavEntry>>> saved_;
    // 確定待ちの戻る/進むの移動先。続く戻る/進むの起点になる。
    std::optional<NavEntry> chained_from_;
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
    std::string target;
};

LinkClickResult ClassifyLinkClick(std::string_view url);
bool IsSafeUrlScheme(std::string_view url) noexcept;
// md4c は URL をエンコードされたまま渡すので、ファイルパスやアンカーとして引く前に通す。
// 16 進 2 桁が続かない % はそのまま残す。
std::string PercentDecode(std::string_view s);
