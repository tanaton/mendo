#pragma once
#include "async_load_coordinator.h"
#include "async_load_result.h"
#include "file_loader.h"
#include "loading_animation.h"
#include "preloader.h"
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <windows.h>

struct Theme;
class TaskScheduler;

// ファイル読み込みのオーケストレーション。
// 内部に LoadingAnimation / Preloader / AsyncLoadCoordinator を持ち、
// UI から見える API はそれらを束ねた薄い facade。preload と StartAsyncLoad の
// 結果ストレージは互いに独立で、TakeAsyncResult/TakeAsyncError が両者を順に確認する。
class FileLoadService {
public:
    constexpr bool IsLoading() const noexcept
    {
        return animation_.IsActive();
    }
    // スピナー非表示でも非同期パースが進行中なら true。ライブリロードのバースト時に
    // 重複スケジューリングを抑制するためのフラグ。preload 経路は preloader_ が状態を持つ。
    bool IsAsyncLoading() const noexcept
    {
        return coordinator_.IsActive() || preloader_.IsActive();
    }
    constexpr float GetLoadingAngle() const noexcept
    {
        return animation_.GetAngle();
    }

    void StartLoading(std::pmr::wstring path);
    // 既に SetLoadingPath 済み (preload 経由など) の状態でアニメーションだけ起動する。
    void BeginLoadingAnimation() noexcept
    {
        animation_.Begin();
    }
    void StopLoading() noexcept;
    void TickLoadingAnimation() noexcept
    {
        animation_.Tick();
    }

    std::expected<Document, FileLoadError> ExecuteLoad();

    // reload_base の意味は AsyncLoadCoordinator::Start を参照。
    void StartAsyncLoad(TaskScheduler& scheduler, HWND hwnd, UINT msg_id, const Theme& theme,
                        std::shared_ptr<const std::pmr::string> reload_base = nullptr);
    // preload 優先。
    std::optional<AsyncLoadResult> TakeAsyncResult();
    // OnParseComplete の null パスで取り出してトースト表示に使う。
    std::optional<FileLoadError> TakeAsyncError() noexcept;
    void CancelAsyncLoad() noexcept
    {
        coordinator_.Cancel();
        preloader_.Cancel();
    }

    // App::Init 前から走らせる経路。
    void StartPreloadAsync(std::pmr::wstring path, Theme theme);

    using PreloadAttachResult = Preloader::AttachResult;

    // App::Init 末尾で呼ぶ。挙動は Preloader::AttachOrApply を参照。
    PreloadAttachResult AttachOrApplyPreload(HWND hwnd, UINT msg_id)
    {
        return preloader_.AttachOrApply(hwnd, msg_id);
    }

#ifdef MENDO_TESTING
    bool PreloadPublishedForTest() const
    {
        return preloader_.HasPublishedForTest();
    }
#endif

    constexpr const std::pmr::wstring& GetLoadingPath() const noexcept
    {
        return loading_path_;
    }
    constexpr void SetLoadingPath(std::pmr::wstring path) noexcept
    {
        loading_path_ = std::move(path);
    }

private:
    LoadingAnimation animation_;
    std::pmr::wstring loading_path_;
    AsyncLoadCoordinator coordinator_;
    Preloader preloader_;
};
