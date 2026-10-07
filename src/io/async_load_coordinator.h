#pragma once
#include "async_load_result.h"
#include "file_loader.h"
#include "worker_latch.h"
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <windows.h>

struct Theme;
class TaskScheduler;

// gen による cancellation、mutex 保護の result/error sink、in_flight フラグを 1 クラスに閉じる。
// 全 public API は UI スレッドからのみ呼ぶ前提 (in_flight_ は非 atomic)。
// worker は gen_ (atomic) のチェックと、mutex 経由の result_/error_ sink への書き込みのみ行う。
class AsyncLoadCoordinator {
public:
    using LoadFileFn = std::expected<LoadedFileDoc, FileLoadError> (*)(const std::pmr::wstring& path, const std::stop_token& stop_token);

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

    // path は worker capture コピーで安全に持ち回す。theme も値コピー: UI スレッドの SetTheme が
    // std::wstring メンバを非アトミックに書き換えるため、参照キャプチャだと EstimateNodeHeights と race。
    // reload_base を渡すと同一ファイルのリロードとして扱い、パース前に旧テキストとの差分判定を行う。
    // 変更なし / 書き込み途中 (prefix shrink) ならパースを省いて判定結果だけ返す。
    void Start(TaskScheduler& scheduler, std::pmr::wstring path, HWND hwnd, UINT msg_id, const Theme& theme,
               std::shared_ptr<const std::pmr::string> reload_base = nullptr);

    // ロード結果の取り出し。fired から TakeResult までの間に Cancel が走った場合 sink は破棄済み。
    std::optional<AsyncLoadResult> TakeResult();
    std::optional<FileLoadError> TakeError() noexcept;

    // gen を進めて worker の以後の publish を弾き、result_/error_ も即時クリア。
    // worker 側 emplace と Cancel reset は同一 mutex 上で直列化される (worker は
    // sink 書き込み前に lock 内で gen を再確認)。
    // request_stop は走行中の parse/Estimate を協調的に打ち切る。
    void Cancel() noexcept
    {
        gen_.fetch_add(1, std::memory_order_relaxed);
        stop_source_.request_stop();
        in_flight_ = false;
        ResetSinks();
    }

private:
    // worker スレッド本体。I/O → リロード差分判定 → Parse → Estimate の各段で gen/stop を確認する。
    void RunWorker(const std::pmr::wstring& path, uint32_t gen, const Theme& theme, const std::stop_token& stop_token,
                   std::shared_ptr<const std::pmr::string> reload_base, HWND hwnd, UINT msg_id);
    static std::expected<LoadedFileDoc, FileLoadError> LoadFromDisk(const std::pmr::wstring& path, const std::stop_token& /*stop_token*/)
    {
        return FileLoader::LoadFile(path);
    }
    bool IsStale(uint32_t gen) const noexcept
    {
        return gen_.load(std::memory_order_relaxed) != gen;
    }

    // sink を swap-out して lock 解放後に破棄する。100MB 級の Document/LayoutCache の
    // destruction を mutex 保持時間に乗せないため。
    void ResetSinks() noexcept;

    LoadFileFn load_file_ = &LoadFromDisk;
    bool in_flight_ = false;
    std::atomic<uint32_t> gen_{ 0 };
    std::mutex mutex_;
    std::optional<AsyncLoadResult> result_;
    std::optional<FileLoadError> error_;

    // request_stop 後は再使用不可なので Start 毎にフレッシュな source へ差し替える。
    std::stop_source stop_source_;

    // dtor で worker 完了を待つ。scheduler_ 共有 worker から self を参照する race を排除する。
    WorkerLatch latch_;
};
