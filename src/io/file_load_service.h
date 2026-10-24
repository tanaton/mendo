#pragma once
#include "async_load_coordinator.h"
#include "async_load_result.h"
#include "file_loader.h"
#include "ui_constants.h"
#include <expected>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <windows.h>

struct Theme;
class TaskScheduler;

class DocumentService {
public:
    static std::expected<Document, FileLoadError> LoadFile(const std::pmr::wstring& path);

    // 非同期ロード経路に乗せる対象か (同期ロードの上限を超えるサイズ)。
    // ファイル取得失敗時 (アクセス不能/仮想パス等) も true を返し、async 経路で reject させる。
    static bool IsAsyncLoadCandidate(const std::pmr::wstring& path) noexcept;
    // ローディングスピナーを表示すべきサイズか。
    static bool ShouldShowLoadingAnimation(const std::pmr::wstring& path) noexcept;
    // size は QueryFileSize の結果。取得失敗 (nullopt) は上限超え扱い。
    static bool IsAsyncLoadCandidate(std::optional<uint64_t> size) noexcept;
    static bool ShouldShowLoadingAnimation(std::optional<uint64_t> size) noexcept;
};

// ファイル読み込みのオーケストレーション。
// スピナーの状態と AsyncLoadCoordinator を持ち、UI から見える API はそれらを束ねた薄い facade。
// 起動時の読み込み (preload) も通知先を後から渡すだけで、同じ AsyncLoadCoordinator を通る。
class FileLoadService {
public:
    constexpr bool IsLoading() const noexcept
    {
        return loading_;
    }
    // スピナー非表示でも非同期パースが進行中なら true。ライブリロードのバースト時に
    // 重複スケジューリングを抑制するためのフラグ。
    bool IsAsyncLoading() const noexcept
    {
        return coordinator_.IsActive();
    }
    constexpr float GetLoadingAngle() const noexcept
    {
        return loading_angle_;
    }

    constexpr void BeginLoadingAnimation() noexcept
    {
        loading_ = true;
        loading_angle_ = 0.0f;
    }
    constexpr void StopLoading() noexcept
    {
        loading_ = false;
    }
    constexpr void TickLoadingAnimation() noexcept
    {
        loading_angle_ += spinner::ROTATION_INCREMENT;
        if (loading_angle_ > TWO_PI) {
            loading_angle_ -= TWO_PI;
        }
    }

    // reload_base の意味は AsyncLoadCoordinator::Start を参照。
    void StartAsyncLoad(TaskScheduler& scheduler, HWND hwnd, UINT msg_id, Theme theme, float content_width,
                        std::shared_ptr<const std::pmr::string> reload_base);
    std::optional<AsyncLoadOutcome> TakeAsync() noexcept
    {
        return coordinator_.Take();
    }
    void CancelAsyncLoad() noexcept
    {
        coordinator_.Cancel();
    }

    // App::Init 前 (ウィンドウを作る前) から走らせる経路。通知先は AttachPreload で渡す。
    void StartPreloadAsync(TaskScheduler& scheduler, std::pmr::wstring path, Theme theme);

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
    bool loading_ = false;
    float loading_angle_ = 0.0f;
    std::pmr::wstring loading_path_;
    AsyncLoadCoordinator coordinator_;
};
