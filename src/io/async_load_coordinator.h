#pragma once
#include "document.h"
#include "document_utils.h"
#include "file_io.h"
#include "file_watcher.h"
#include "layout_cache.h"
#include "worker_latch.h"
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <windows.h>

// 同一ファイルのリロード判定。旧/新の UTF-8 テキストを比べてリロード方針と差分位置 (UTF-8 byte オフセット) を決める。

// 新旧コンテンツの最初の差分 UTF-8 byte 位置を返す。同一の場合は npos。
// 前提: 両テキストとも LF 正規化済みであること。old は DocumentData::raw、new は LoadFile 由来で
// どちらも正規化済み (issue #273)。
size_t FindFirstDifference(std::string_view old_text, std::string_view new_text) noexcept;

// 同一ファイルのリロード時に取るべき操作を示す。
enum class ReloadOp : uint8_t {
    NoChange,          // 差分なし。リロード不要。
    DeferPrefixShrink, // prefix-only shrink。truncate→rewrite の前半として defer。
    Replace,           // 再パースした文書に差し替え、差分位置へスクロールする。
};

struct ReloadDecision {
    ReloadOp op;
    size_t diff_pos; // NoChange のとき std::string_view::npos、それ以外は差分開始位置。
};

// 旧/新コンテンツの UTF-8 テキストを比較し、リロード方針を決定する純粋関数。
// accept_prefix_shrink なら prefix shrink も defer せず Replace にする (PrefixShrinkRetry の上限後)。
ReloadDecision AnalyzeReloadDiff(std::string_view old_text, std::string_view new_text, bool accept_prefix_shrink) noexcept;

// 末尾を削っただけの保存は何度読み直しても prefix shrink になるため、defer 回数に上限を設け、
// 超えたら縮んだ内容を確定版として受け入れる。
class PrefixShrinkRetry {
public:
    static constexpr uint8_t kMaxDefers = 10;

    void CountDefer() noexcept
    {
        if (!Exhausted()) {
            ++count_;
        }
    }
    bool Exhausted() const noexcept
    {
        return count_ >= kMaxDefers;
    }
    void Reset() noexcept
    {
        count_ = 0;
    }

private:
    uint8_t count_ = 0;
};

struct Theme;
class TaskScheduler;

// LoadFile が返す UTF-8 ドキュメントテキスト (BOM 除去・LF 正規化済) + 元の UTF-8 バイト数 (BOM 込み)。
// byte_size はリロード時の二段階保存検出 (IsFileLargerThan) や AnalyzeReloadDiff の参照用。
struct LoadedFileDoc {
    std::string text;
    size_t byte_size = 0;
};

// ファイルを ReadFile で string バッファへ直接読み込み、UTF-8 BOM を除去して LF に正規化して返す。
// Document とリロード diff はこの出力が LF-only であることを前提にする (issue #273)。
// wstring 経由の二重変換 (UTF-8 → wstring → UTF-8) を行わないため巨大ファイルで高速。
// stop_token は AsyncLoadCoordinator::LoadFileFn に合わせて受けるだけで、読み込みの途中では止めない。
std::expected<LoadedFileDoc, FileLoadError> LoadFile(const std::wstring& path, const std::stop_token& stop_token);

// 表示中のファイルを読み直すとき (開き直しを含む) に worker に渡す旧文書。原文はパース前の差分判定に、
// ノードはパース後に変わらなかったノードを数えるのに使う。空ならリロードとして扱わない。
struct ReloadBase {
    std::shared_ptr<const DocumentData> data;
    // prefix shrink も defer せずにパースする (PrefixShrinkRetry の上限後)。
    bool accept_prefix_shrink = false;
};

// 同一ファイルのリロードで、worker がパース前に差分判定まで済ませた結果。
// NoChange / DeferPrefixShrink と書き込み途中はパースせずに返す (doc は空)。
// 100MB 級の文書はテキスト・runs・リンク URL を arena ごと捨てても、数十万ノードの走査と
// 表等の解放で UI を十数 ms 止めるため、これ以上のノード数なら worker で破棄する (DisposeOnWorkerIfLarge)。
inline constexpr size_t kDisposeMinNodes = 4096;

struct ReloadCheck {
    // 書き込み途中 (still_growing) なら判定せず既定値のまま。
    ReloadDecision decision{ ReloadOp::NoChange, std::string_view::npos };
    // 判定に使った旧文書。UI 側の文書がこれと同一のときだけ decision と ends を信用できる (同一ならパスも同じ)。
    // 弱参照なので、判定後に UI が文書を差し替えれば旧文書はその時点で解放される。
    std::weak_ptr<const DocumentData> base;
    // 旧文書のノードと比べて変わらなかった先頭と末尾のノード数 (Replace のとき)。大きな文書で UI を止めないよう worker で数える。
    // 数えられないのはパース中に UI が旧文書を手放したときだけなので、base が UI の文書と同一なら必ず数えてある。
    UnchangedEnds ends;
    // 読み終えた後もファイルが伸びている (エディタの書き込み途中)。
    // UI スレッドはファイルに触れない (ネットワーク上のファイルで止まる) ので、結果を出す直前に worker で確かめる。
    bool still_growing = false;
};

// ワーカースレッドでのパース結果。cache は doc 向けに EstimateNodeHeights 済み。
struct AsyncLoadResult {
    Document doc;
    LayoutCache cache;
    std::optional<ReloadCheck> reload;
    // 開いた文書の変更の監視に使う。リロードは今の監視を続けるので作らない。
    FileWatchTarget watch;
};

// 非同期ロード 1 回分の結果。成功時は AsyncLoadResult、失敗時はその理由。
using AsyncLoadOutcome = std::expected<AsyncLoadResult, FileLoadError>;

// stop_token による cancellation、mutex 保護の結果 sink、in_flight フラグを 1 クラスに閉じる。
// 全 public API は UI スレッドからのみ呼ぶ前提 (in_flight_ は非 atomic)。
// worker は自分の stop_token のチェックと、mutex 経由の outcome_ sink への書き込みのみ行う。
// Start と Cancel は必ず旧 source に request_stop するので、停止を要求された token を持つ worker が古い worker。
class AsyncLoadCoordinator {
public:
    using LoadFileFn = std::expected<LoadedFileDoc, FileLoadError> (*)(const std::wstring& path, const std::stop_token& stop_token);

    AsyncLoadCoordinator() = default;
    // テストが worker を読み込み中で止め、Cancel / 再 Start との競合を決定的に再現するための差し替え口。
    explicit AsyncLoadCoordinator(LoadFileFn load_file) noexcept : load_file_(load_file)
    {}
    // dtor で走行中 worker の完了を待つ。OnDestroy を経ない経路でも UAF を起こさないため。
    ~AsyncLoadCoordinator();
    AsyncLoadCoordinator(const AsyncLoadCoordinator&) = delete;
    AsyncLoadCoordinator& operator=(const AsyncLoadCoordinator&) = delete;

    bool IsActive() const noexcept
    {
        return in_flight_;
    }
    // 最後に Start したパス。IsActive の間だけ意味を持つ。
    const std::wstring& LoadingPath() const noexcept
    {
        return loading_path_;
    }

    // path は worker capture コピーで安全に持ち回す。theme も値コピー: UI スレッドの SetTheme が
    // std::wstring メンバを非アトミックに書き換えるため、参照キャプチャだと EstimateNodeHeights と race。
    // reload_base を渡すと同一ファイルのリロードとして扱い、パース前に旧テキストとの差分判定を行う。
    // 変更なし / 書き込み途中 (prefix shrink) ならパースを省いて判定結果だけ返す。
    // 結果を出したら Attach で渡した通知先へ送る。Attach の前なら送らない (起動時にウィンドウを作るのと並行して読み込む)。
    // content_width は推定に使う本文幅 (0 は幅を知らない)。
    void Start(TaskScheduler& scheduler, std::wstring path, Theme theme, float content_width = 0.0f, ReloadBase reload_base = {});

    // 通知先を渡す。以後の Start の結果もここへ送る。読み込み中なら true で、結果が出ていればこの場で通知を送る。
    bool Attach(HWND hwnd, UINT msg_id);

    // ロード結果の取り出し。fired から Take までの間に Cancel が走った場合 sink は破棄済み。
    std::optional<AsyncLoadOutcome> Take() noexcept;

    // request_stop で走行中の parse/Estimate を協調的に打ち切り、worker の以後の publish を弾く。outcome_ も即時クリア。
    // worker 側 emplace と Cancel reset は同一 mutex 上で直列化される (worker は
    // sink 書き込み前に lock 内で stop を再確認)。
    void Cancel() noexcept
    {
        stop_source_.request_stop();
        in_flight_ = false;
        ResetSinks();
    }

private:
    // worker スレッド本体。I/O → リロード差分判定 → Parse → Estimate の各段で stop を確認する。
    void RunWorker(const std::wstring& path, const Theme& theme, float content_width,
                   const std::stop_token& stop_token, ReloadBase reload_base);
    // sink 書き込みは Cancel との直列化のため lock 内で stop を再確認してから行う。
    void Publish(const std::stop_token& stop_token, AsyncLoadOutcome outcome);
    void NotifyLocked() const noexcept;

    // sink を swap-out して mutex の外で破棄する。大きな文書は最後に Start した scheduler の worker に回す。
    void ResetSinks() noexcept;

    LoadFileFn load_file_ = &LoadFile;
    // 最後に Start で渡された scheduler。worker の完了を待つ (latch_) ので coordinator より長く生きる。
    TaskScheduler* scheduler_ = nullptr;
    bool in_flight_ = false;
    std::wstring loading_path_;
    std::mutex mutex_;
    std::optional<AsyncLoadOutcome> outcome_;
    // 結果の通知先。worker が Publish で読むので mutex_ で守る。
    HWND notify_hwnd_ = nullptr;
    UINT notify_msg_ = 0;

    // request_stop 後は再使用不可なので Start 毎にフレッシュな source へ差し替える。
    std::stop_source stop_source_;

    // dtor で worker 完了を待つ。scheduler_ 共有 worker から self を参照する race を排除する。
    WorkerLatch latch_;
};
