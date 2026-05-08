#pragma once
#include "async_load_result.h"
#include "file_loader.h"
#include "task_scheduler.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <windows.h>

struct Theme;

// StartAsyncLoad 専用の状態管理を Preloader と対称に分離したもの。
// gen による cancellation, mutex 保護の result/error sink, in_flight フラグを 1 クラスに閉じる。
// FileLoadService からは合成だけ受け持ち、preloader_ と並べて TakeResult/TakeError を順に確認する。
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

    // 既存リクエストを破棄。worker は次の gen チェックで早期 return する (PostMessage は飛ばない)。
    void Cancel() noexcept
    {
        gen_.fetch_add(1, std::memory_order_relaxed);
        in_flight_ = false;
    }

private:
    void ResetSinks();

    bool in_flight_ = false;
    std::atomic<uint32_t> gen_{ 0 };
    std::mutex mutex_;
    std::optional<AsyncLoadResult> result_;
    std::optional<FileLoadError> error_;
};
