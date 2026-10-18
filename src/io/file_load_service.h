#pragma once
#include "async_load_coordinator.h"
#include "async_load_result.h"
#include "file_loader.h"
#include "ui_constants.h"
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <windows.h>

// ローディングスピナーの回転角度と表示有効/無効だけを管理する薄いクラス。
// FileLoadService の async/preload 状態とは独立して動き、テスト時にも純粋関数的に扱える。
class LoadingAnimation {
public:
    constexpr bool IsActive() const noexcept
    {
        return active_;
    }
    constexpr float GetAngle() const noexcept
    {
        return angle_;
    }

    constexpr void Begin() noexcept
    {
        active_ = true;
        angle_ = 0.0f;
    }
    constexpr void End() noexcept
    {
        active_ = false;
    }
    constexpr void Tick() noexcept
    {
        angle_ += spinner::ROTATION_INCREMENT;
        if (angle_ > TWO_PI) {
            angle_ -= TWO_PI;
        }
    }

private:
    bool active_ = false;
    float angle_ = 0.0f;
};

struct Theme;
class TaskScheduler;

// ファイル読み込みのオーケストレーション。
// 内部に LoadingAnimation / AsyncLoadCoordinator を持ち、UI から見える API はそれらを束ねた薄い facade。
// 起動時の読み込み (preload) も通知先を後から渡すだけで、同じ AsyncLoadCoordinator を通る。
class FileLoadService {
public:
    constexpr bool IsLoading() const noexcept
    {
        return animation_.IsActive();
    }
    // スピナー非表示でも非同期パースが進行中なら true。ライブリロードのバースト時に
    // 重複スケジューリングを抑制するためのフラグ。
    bool IsAsyncLoading() const noexcept
    {
        return coordinator_.IsActive();
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
    std::optional<AsyncLoadOutcome> TakeAsync() noexcept
    {
        return coordinator_.Take();
    }
    void CancelAsyncLoad() noexcept
    {
        coordinator_.Cancel();
    }

    // App::Init 前 (ウィンドウを作る前) から走らせる経路。通知先は AttachPreload で渡す。
    void StartPreloadAsync(TaskScheduler& scheduler, std::pmr::wstring path, const Theme& theme);

    using PreloadAttachResult = AsyncLoadCoordinator::AttachResult;

    // App::LoadInitialDocument で呼ぶ。挙動は AsyncLoadCoordinator::Attach を参照。
    PreloadAttachResult AttachPreload(HWND hwnd, UINT msg_id)
    {
        return coordinator_.Attach(hwnd, msg_id);
    }

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
};
