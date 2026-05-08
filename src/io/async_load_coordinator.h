#pragma once
#include "async_load_result.h"
#include "file_loader.h"
#include "task_scheduler.h"
#include <atomic>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <string>
#include <windows.h>

struct Theme;

// gen による cancellation、mutex 保護の result/error sink、in_flight フラグを 1 クラスに閉じる。
// 全 public API は UI スレッドからのみ呼ぶ前提 (in_flight_ は非 atomic)。
// worker は gen_ (atomic) のチェックと、mutex 経由の result_/error_ sink への書き込みのみ行う。
class AsyncLoadCoordinator {
public:
    AsyncLoadCoordinator() = default;
    AsyncLoadCoordinator(const AsyncLoadCoordinator&) = delete;
    AsyncLoadCoordinator& operator=(const AsyncLoadCoordinator&) = delete;

    bool IsActive() const noexcept
    {
        return in_flight_;
    }

    // path は worker capture コピーで安全に持ち回す。theme も値コピー: UI スレッドの SetTheme が
    // std::wstring メンバを非アトミックに書き換えるため、参照キャプチャだと EstimateNodeHeights と race。
    void Start(TaskScheduler& scheduler, std::pmr::wstring path, HWND hwnd, UINT msg_id, const Theme& theme);

    // ロード結果の取り出し。fired から TakeResult までの間に Cancel が走った場合 sink は破棄済み。
    std::optional<AsyncLoadResult> TakeResult();
    std::optional<FileLoadError> TakeError() noexcept;

    // 既存リクエストを破棄。gen を進めて worker の次回 gen チェックで早期 return させ、
    // 同時に mutex 下で result_/error_ もクリアする。worker が最終 gen check を通過した
    // 直後に Cancel が割り込んだ場合でも、sink への emplace と Cancel の reset は同じ
    // mutex 上で直列化される (worker は emplace 前に lock 内で gen 再チェック)。
    void Cancel() noexcept
    {
        gen_.fetch_add(1, std::memory_order_relaxed);
        in_flight_ = false;
        const std::lock_guard lock(mutex_);
        result_.reset();
        error_.reset();
    }

private:
    void ResetSinks();

    bool in_flight_ = false;
    std::atomic<uint32_t> gen_{ 0 };
    std::mutex mutex_;
    std::optional<AsyncLoadResult> result_;
    std::optional<FileLoadError> error_;
};
